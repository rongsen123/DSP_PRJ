#include "DSP28x_Project.h"
#include "sci.h"
#include "adc.h"
#include "cpld_link.h"
#include "grid_pll.h"
#include "statcom_runtime.h"
/*
 * 系统参数
 */
#define SCI_LSPCLK_HZ              22500000UL //低速时钟90M/4=22.5M
#define SCI_FIFO_DEPTH             16       //SCI通信FIFO深度
#define SCI_GAP_TIMER_TICKS        157500UL // 1.75 ms at 90 MHz
#define CPLD_POLL_PERIOD_MS        20       //请求CPLD指令周期20ms
#define CPLD_RESPONSE_TIMEOUT_MS   20       //CPLD回复周期20ms
#define CPLD_OFFLINE_TIMEOUT_MS    500      //超时500ms认为掉线
#define CPLD_MAX_RETRIES           2        //两次收不到回复就认为掉线
#define SCI_RX_RING_MASK           (MODBUS_FRAME_MAX - 1)       //数据最大数量
/*
 * 状态机
 */
#define MASTER_REQUEST_NONE        0
#define MASTER_REQUEST_READ        1
#define MASTER_REQUEST_COMMAND     2
#define MASTER_REQUEST_CLEAR_FAULT 3
#define MASTER_REQUEST_RESET_CPLD  4
/*
 * 串口接收数据帧结构体
 */
typedef struct
{
    volatile Uint8 ring[MODBUS_FRAME_MAX];  //8字节数据存储
    volatile Uint16 head;
    volatile Uint16 tail;
    volatile Uint32 last_timer;
} SCI_RX_PORT;

/*
 * 结构体和参数初始化
 */
volatile Uint32 system_millis = 0;
volatile MODBUS_LINK_STATS modbus_cpld_stats = {0,0,0,0,0,0,0};
volatile MODBUS_LINK_STATS modbus_pc_stats = {0,0,0,0,0,0,0};
/*
 * 变量声明
 */
static SCI_RX_PORT sci_a_rx;
static SCI_RX_PORT sci_b_rx;
static Uint16 master_waiting = 0;
static Uint16 master_request = MASTER_REQUEST_NONE;
static Uint16 master_retries = 0;
static Uint16 master_command_value = 0;
static Uint32 master_sent_ms = 0;
static Uint32 master_next_poll_ms = 0;
static Uint32 cpld_last_valid_ms = 0;
static volatile Uint16 dsp_reset_pending = 0;
/*
 * 中断服务函数声明
 */
interrupt void CommunicationTimer0Isr(void);
interrupt void ModbusSciARxIsr(void);
interrupt void ModbusSciBRxIsr(void);

static void sci_gpio_init(void);
static void sci_modules_init(void);
static void sci_timer_init(void);
static Uint16 sci_push_rx(SCI_RX_PORT *port, Uint16 word,volatile MODBUS_LINK_STATS *stats);
static void sci_discard_rx_port(SCI_RX_PORT *port);
static Uint16 sci_take_frame(SCI_RX_PORT *port, Uint8 *frame);
static void sci_write_a(const Uint8 *data, Uint16 length);
static void sci_write_b(const Uint8 *data, Uint16 length);
static Uint16 modbus_frame_valid(const Uint8 *frame, Uint16 length);
static Uint16 get_be16(const Uint8 *data);
static void put_be16(Uint8 *data, Uint16 value);
static void append_crc(Uint8 *frame, Uint16 payload_length);
static void master_send_read(void);
static void master_send_command(Uint16 command);
static void master_send_clear_fault(void);
static void master_send_reset_cpld(void);
static void master_resend(void);
static void master_handle_frame(const Uint8 *frame, Uint16 length);
static void master_service(void);
static Uint16 dsp_input_register(Uint16 address, Uint16 *value);
static Uint16 dsp_holding_register(Uint16 address, Uint16 *value);
static Uint16 dsp_write_holding(Uint16 address, Uint16 value);
static void dsp_software_reset(void);
static void slave_exception(Uint8 function, Uint8 exception);
static void slave_handle_frame(const Uint8 *frame, Uint16 length);

/*
 * CRC16计算函数
 * 输入:数据帧、长度
 * 输出:16bitCRC
 */
Uint16 Modbus_Crc16(const Uint8 *data, Uint16 length)
{
    Uint16 crc = 0xFFFF;    //CRC初始值
    Uint16 index;           //计数器
    Uint16 bit;             //8bit计数

    for(index = 0; index < length; index++)
    {
        crc ^= (Uint16)(data[index] & 0x00FF);// CRC=CRC^data 只取低8位
        for(bit = 0; bit < 8; bit++)
        {
            if((crc & 0x0001) != 0)
            {
                crc = (crc >> 1) ^ 0xA001;    //多项式处理
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc; //最终返回计算好的CRC
}
/*
 * 大端顺序拼接函数
 * 输入:2个8bit数据
 * 输出:1个16bit数据
 */
static Uint16 get_be16(const Uint8 *data)
{
    return (Uint16)(((Uint16)data[0] << 8) | (Uint16)data[1]);
}
/*
 * 大端顺序分解函数
 * 输入:1个16bit数据
 * 输出:2个8bit数据
 */
static void put_be16(Uint8 *data, Uint16 value)
{
    data[0] = (Uint8)((value >> 8) & 0x00FFU);
    data[1] = (Uint8)(value & 0x00FFU);
}
/*
 * CRC赋值函数
 * 输入:数据帧结构体、数据长度
 * 输出:无
 */

static void append_crc(Uint8 *frame, Uint16 payload_length)
{
    Uint16 crc = Modbus_Crc16(frame, payload_length);   //计算数据帧的16bitCRC
    frame[payload_length] = (Uint8)(crc & 0x00FFU);     //先低8位
    frame[payload_length + 1] = (Uint8)((crc >> 8) & 0x00FFU);     //再高八位
}
/*
 * 判断数据帧CRC是否正确函数
 * 输入:缓存数据帧结构体、长度
 * 输出:0——该数据帧CRC无效
 *      1——该数据帧CRC有效
 */
static Uint16 modbus_frame_valid(const Uint8 *frame, Uint16 length)
{
    Uint16 received;    //数据中解析出来的CRC
    Uint16 calculated;  //结合数据进行计算得到的CRC

    if(length < 4)
    {
        return 0;
    }
    /*
     * 将数据帧结构体中的最后2个字节拼接起来
     * {frame[length - 1],frame[length - 2]}
     */
    received = (Uint16)frame[length - 2] |  ((Uint16)frame[length - 1] << 8);
    calculated = Modbus_Crc16(frame, length - 2);
    return (received == calculated) ? 1 : 0;
}

/*
 * SCI串口初始化函数
 * TXA:GPIO12 RXA:GPIO7
 * TXB:GPIO22 RXA:GPIO23
 * 打开复用功能
 */

static void sci_gpio_init(void)
{
    EALLOW;
    GpioCtrlRegs.GPAPUD.bit.GPIO12 = 0;
    GpioCtrlRegs.GPAPUD.bit.GPIO7 = 0;
    GpioCtrlRegs.GPAPUD.bit.GPIO22 = 0;
    GpioCtrlRegs.GPAPUD.bit.GPIO23 = 0;
    GpioCtrlRegs.GPAQSEL1.bit.GPIO7 = 3;
    GpioCtrlRegs.GPAQSEL2.bit.GPIO23 = 3;
    GpioCtrlRegs.GPAMUX1.bit.GPIO12 = 2;
    GpioCtrlRegs.GPAMUX1.bit.GPIO7 = 2;
    GpioCtrlRegs.GPAMUX2.bit.GPIO22 = 3;
    GpioCtrlRegs.GPAMUX2.bit.GPIO23 = 3;
    EDIS;
}
/*
 * 串口功能初始化
 */
static void sci_modules_init(void)
{
    Uint32 divider_a;
    Uint32 divider_b;
    Uint16 high_a;
    Uint16 low_a;
    Uint16 high_b;
    Uint16 low_b;

    //波特率计算

    divider_a = ((SCI_LSPCLK_HZ + (MODBUS_SCIA_BAUD_RATE * 4UL)) /
                (MODBUS_SCIA_BAUD_RATE * 8UL)) - 1UL;
    divider_b = ((SCI_LSPCLK_HZ + (MODBUS_SCIB_BAUD_RATE * 4UL)) /
                (MODBUS_SCIB_BAUD_RATE * 8UL)) - 1UL;
    high_a = (Uint16)((divider_a >> 8) & 0x00FFUL);
    low_a = (Uint16)(divider_a & 0x00FFUL);
    high_b = (Uint16)((divider_b >> 8) & 0x00FFUL);
    low_b = (Uint16)(divider_b & 0x00FFUL);
    //打开工作时钟
    EALLOW;
    SysCtrlRegs.PCLKCR0.bit.SCIAENCLK = 1;
    SysCtrlRegs.PCLKCR0.bit.SCIBENCLK = 1;
    EDIS;

    sci_gpio_init();
    //串口基本配置、bit数、停止位、起始位、波特率
    SciaRegs.SCICTL1.all = 0x0003;
    SciaRegs.SCICCR.all = 0x0007;
    SciaRegs.SCICTL2.all = 0x0002;
    SciaRegs.SCIHBAUD = high_a;
    SciaRegs.SCILBAUD = low_a;
    //串口a中断、FIFO深度
    SciaRegs.SCIFFTX.all = 0xE040;
    SciaRegs.SCIFFRX.all = 0x2061;
    SciaRegs.SCIFFCT.all = 0x0000;
    SciaRegs.SCICTL1.all = 0x0023;
    SciaRegs.SCIFFTX.bit.TXFIFOXRESET = 1;
    SciaRegs.SCIFFRX.bit.RXFIFORESET = 1;
    SciaRegs.SCIPRI.bit.FREE = 1;
    //串口b中断、FIFO深度
    ScibRegs.SCICTL1.all = 0x0003;
    ScibRegs.SCICCR.all = 0x0007;
    ScibRegs.SCICTL2.all = 0x0002;
    ScibRegs.SCIHBAUD = high_b;
    ScibRegs.SCILBAUD = low_b;
    ScibRegs.SCIFFTX.all = 0xE040;
    ScibRegs.SCIFFRX.all = 0x2061;
    ScibRegs.SCIFFCT.all = 0x0000;
    ScibRegs.SCICTL1.all = 0x0023;
    ScibRegs.SCIFFTX.bit.TXFIFOXRESET = 1;
    ScibRegs.SCIFFRX.bit.RXFIFORESET = 1;
    ScibRegs.SCIPRI.bit.FREE = 1;
    //串口服务函数
    EALLOW;
    PieVectTable.SCIRXINTA = &ModbusSciARxIsr;
    PieVectTable.SCIRXINTB = &ModbusSciBRxIsr;
    EDIS;
    //打开串口对应中断
    PieCtrlRegs.PIEIER9.bit.INTx1 = 1;
    PieCtrlRegs.PIEIER9.bit.INTx3 = 1;
}
/*
 * 定时器中断初始化
 */
static void sci_timer_init(void)
{
    InitCpuTimers();
    ConfigCpuTimer(&CpuTimer0, 90.0, 1000.0);   //90k
    //定时器中断服务函数
    EALLOW;
    PieVectTable.TINT0 = &CommunicationTimer0Isr;
    EDIS;
    PieCtrlRegs.PIEIER1.bit.INTx7 = 1;

    CpuTimer1Regs.TCR.bit.TSS = 1;
    CpuTimer1Regs.PRD.all = 0xFFFFFFFF;
    CpuTimer1Regs.TPR.all = 0;
    CpuTimer1Regs.TPRH.all = 0;
    CpuTimer1Regs.TCR.bit.TRB = 1;
    CpuTimer1Regs.TCR.bit.TIE = 0;
    CpuTimer1Regs.TCR.bit.TSS = 0;
    CpuTimer0Regs.TCR.all = 0x4001;
}
/*
 * 结构体初始化赋值函数
 */
void communication_init(void)
{
    sci_a_rx.head = 0;
    sci_a_rx.tail = 0;
    sci_b_rx.head = 0;
    sci_b_rx.tail = 0;
    sci_timer_init();
    sci_modules_init();
    IER |= M_INT9;
}
/*
 * 定时器中断服务函数
 * 90k进一次
 */
interrupt void CommunicationTimer0Isr(void)
{
    system_millis++;
    PieCtrlRegs.PIEACK.all = PIEACK_GROUP1;
}
/*
 * 环形缓存区
 * 输入:串口接收结构体、串口接收字节、系统状态结构体
 * 输出:将串口接收字节缓存到ring数组中
 *      越界overflows++
 */
static Uint16 sci_push_rx(SCI_RX_PORT *port, Uint16 word, volatile MODBUS_LINK_STATS *stats)
{
    Uint16 next = (port->head + 1) & SCI_RX_RING_MASK;
    port->last_timer = CpuTimer1Regs.TIM.all;
    if(next != port->tail)
    {
        port->ring[port->head] = (Uint8)(word & 0x00FF);
        port->head = next;
        return 1U;
    }
    else
    {
        stats->overflows++;
        return 0U;
    }
}

/*
 * 丢弃当前正在接收的残帧。
 * SCI出现硬件错误后，错误前后的字节不能再拼成一个Modbus RTU帧。
 */
static void sci_discard_rx_port(SCI_RX_PORT *port)
{
    port->tail = port->head;
    port->last_timer = CpuTimer1Regs.TIM.all;
}
/*
 * 串口A接收中断服务函数
 *
 */

interrupt void ModbusSciARxIsr(void)
{
    Uint16 rx_status;
    Uint16 word;
    Uint16 rx_error = SciaRegs.SCIFFRX.bit.RXFFOVF;

    if(rx_error != 0U)
    {
        modbus_cpld_stats.overflows++;
    }

    while(SciaRegs.SCIFFRX.bit.RXFFST != 0) //FIFO已经有字节
    {
        rx_status = SciaRegs.SCIRXST.all;
        word = SciaRegs.SCIRXBUF.all;

        /* SCIRXST: PE/OE/FE/BRKDT/RXERROR；RXBUF[15:14]: PE/FE。 */
        if(((rx_status & 0x00BCU) != 0U) || ((word & 0xC000U) != 0U))
        {
            rx_error = 1U;
            modbus_cpld_stats.format_errors++;
        }
        else if(rx_error == 0U)
        {
            if(sci_push_rx(&sci_a_rx, word, &modbus_cpld_stats) == 0U) //进入环形缓存区
            {
                rx_error = 1U;
            }
        }
    }

    if(rx_error != 0U)
    {
        sci_discard_rx_port(&sci_a_rx);
        SciaRegs.SCICTL1.bit.SWRESET = 0;
        SciaRegs.SCIFFRX.bit.RXFIFORESET = 0;
        SciaRegs.SCIFFRX.bit.RXFIFORESET = 1;
        SciaRegs.SCICTL1.bit.SWRESET = 1;
    }

    SciaRegs.SCIFFRX.bit.RXFFOVRCLR = 1;
    SciaRegs.SCIFFRX.bit.RXFFINTCLR = 1;
    PieCtrlRegs.PIEACK.all = PIEACK_GROUP9;
}

/*
 * 串口BA接收中断服务函数
 *
 */

interrupt void ModbusSciBRxIsr(void)
{
    Uint16 rx_status;
    Uint16 word;
    Uint16 rx_error = ScibRegs.SCIFFRX.bit.RXFFOVF;

    if(rx_error != 0U)
    {
        modbus_pc_stats.overflows++;
    }

    while(ScibRegs.SCIFFRX.bit.RXFFST != 0)
    {
        rx_status = ScibRegs.SCIRXST.all;
        word = ScibRegs.SCIRXBUF.all;

        /* SCIRXST: PE/OE/FE/BRKDT/RXERROR；RXBUF[15:14]: PE/FE。 */
        if(((rx_status & 0x00BCU) != 0U) || ((word & 0xC000U) != 0U))
        {
            rx_error = 1U;
            modbus_pc_stats.format_errors++;
        }
        else if(rx_error == 0U)
        {
            if(sci_push_rx(&sci_b_rx, word, &modbus_pc_stats) == 0U)
            {
                rx_error = 1U;
            }
        }
    }

    if(rx_error != 0U)
    {
        sci_discard_rx_port(&sci_b_rx);
        ScibRegs.SCICTL1.bit.SWRESET = 0;
        ScibRegs.SCIFFRX.bit.RXFIFORESET = 0;
        ScibRegs.SCIFFRX.bit.RXFIFORESET = 1;
        ScibRegs.SCICTL1.bit.SWRESET = 1;
    }

    ScibRegs.SCIFFRX.bit.RXFFOVRCLR = 1;
    ScibRegs.SCIFFRX.bit.RXFFINTCLR = 1;
    PieCtrlRegs.PIEACK.all = PIEACK_GROUP9;
}
/*
 * 与sci_push_rx()配套使用
 * 将环形数据帧结构体数据取出来
 */
static Uint16 sci_take_frame(SCI_RX_PORT *port, Uint8 *frame)
{
    Uint16 count = 0;
    Uint32 elapsed;
    //写指针=读指针——>无数据可读
    if(port->head == port->tail)
    {
        return 0;
    }
    elapsed = port->last_timer - CpuTimer1Regs.TIM.all;
    if(elapsed < SCI_GAP_TIMER_TICKS)
    {
        return 0;
    }
    //关闭中断 临界区:防止数据被篡改
    DINT;
    while((port->tail != port->head) && (count < MODBUS_FRAME_MAX))
    {
        frame[count] = port->ring[port->tail];
        port->tail = (port->tail + 1) & SCI_RX_RING_MASK;
        count++;
    }
    EINT;//打开中断
    return count;
}
/*
 * 串口A发送函数
 */
static void sci_write_a(const Uint8 *data, Uint16 length)
{
    Uint16 index;

    /* Begin each CPLD request from a known empty TX FIFO state. */
    SciaRegs.SCIFFTX.bit.TXFIFOXRESET = 0;
    SciaRegs.SCIFFTX.bit.TXFFINTCLR = 1;
    SciaRegs.SCIFFTX.bit.TXFIFOXRESET = 1;

    for(index = 0; index < length; index++)
    {
        /* Keep SCI-A RX active because the CPLD may reply immediately. */
        while(SciaRegs.SCIFFTX.bit.TXFFST != 0U)
        {
        }

        SciaRegs.SCITXBUF = (Uint16)(data[index] & 0x00FFU);
    }

    while(SciaRegs.SCICTL2.bit.TXEMPTY == 0U)
    {
    }

    modbus_cpld_stats.tx_frames++;
}
/*
 * 串口B发送函数
 */
static void sci_write_b(const Uint8 *data, Uint16 length)
{
    Uint16 index;

    /* Disable RX while replying so external/local echo cannot disturb TX. */
    ScibRegs.SCIFFRX.bit.RXFFIENA = 0;

    /* Begin each response from a known empty TX FIFO state. */
    ScibRegs.SCIFFTX.bit.TXFIFOXRESET = 0;
    ScibRegs.SCIFFTX.bit.TXFFINTCLR = 1;
    ScibRegs.SCIFFTX.bit.TXFIFOXRESET = 1;

    for(index = 0; index < length; index++)
    {
        /* TI echoback polling method: queue the next byte when FIFO is empty. */
        while(ScibRegs.SCIFFTX.bit.TXFFST != 0U)
        {
        }

        ScibRegs.SCITXBUF = (Uint16)(data[index] & 0x00FFU);
    }

    /* Wait until the final byte has left the transmit shift register. */
    while(ScibRegs.SCICTL2.bit.TXEMPTY == 0U)
    {
    }

    /* Remove any reply echo before accepting the next Modbus request. */
    ScibRegs.SCIFFRX.bit.RXFIFORESET = 0;
    ScibRegs.SCIFFRX.bit.RXFFOVRCLR = 1;
    ScibRegs.SCIFFRX.bit.RXFFINTCLR = 1;
    ScibRegs.SCIFFRX.bit.RXFIFORESET = 1;
    ScibRegs.SCIFFRX.bit.RXFFIENA = 1;

    modbus_pc_stats.tx_frames++;
}
/*
 * 主站主动轮询从站函数
 */
static void master_send_read(void)
{
    Uint8 frame[8];
    frame[0] = MODBUS_CPLD_ADDRESS;//地址
    frame[1] = 0x04;    //功能码:读取
    put_be16(&frame[2], 0x0000);    //起始寄存器地址
    put_be16(&frame[4], 0x000E);    //读取0000-000D，共14个寄存器
    append_crc(frame, 6);   //CRC计算 幅值
    sci_write_a(frame, 8);  //SCI发送
    master_waiting = 1; //主站等待状态
    master_request = MASTER_REQUEST_READ;   //切换到读取状态机
    master_sent_ms = system_millis; //发送时间
}
/*
 * 主站主动写从站函数
 */
static void master_send_command(Uint16 command)
{
    Uint8 frame[8];
    frame[0] = MODBUS_CPLD_ADDRESS;//从站地址
    frame[1] = 0x06;    //06命令 写单个寄存器
    put_be16(&frame[2], 0x0100);    //特定地址
    put_be16(&frame[4], command);   //启动等命令
    append_crc(frame, 6);
    sci_write_a(frame, 8);
    master_command_value = command;
    master_waiting = 1;
    master_request = MASTER_REQUEST_COMMAND;
    master_sent_ms = system_millis;
}
/*
 * 向CPLD写入一次性故障清除密钥。CPLD只在STOP回显状态接受该命令，
 * 并且只清除已经消失的锁存故障源。
 */
static void master_send_clear_fault(void)
{
    Uint8 frame[8];
    frame[0] = MODBUS_CPLD_ADDRESS;
    frame[1] = 0x06;
    put_be16(&frame[2], 0x0101);
    put_be16(&frame[4], MODBUS_CLEAR_FAULT_KEY);
    append_crc(frame, 6);
    sci_write_a(frame, 8);
    master_waiting = 1;
    master_request = MASTER_REQUEST_CLEAR_FAULT;
    master_sent_ms = system_millis;
}

/* Ask the CPLD to reset its internal communication, ADC and fault state. */
static void master_send_reset_cpld(void)
{
    Uint8 frame[8];
    frame[0] = MODBUS_CPLD_ADDRESS;
    frame[1] = 0x06;
    put_be16(&frame[2], 0x0102);
    put_be16(&frame[4], MODBUS_RESET_CPLD_KEY);
    append_crc(frame, 6);
    sci_write_a(frame, 8);
    master_waiting = 1;
    master_request = MASTER_REQUEST_RESET_CPLD;
    master_sent_ms = system_millis;
}
/*
 * 根据状态机决定主站写还是读
 */
static void master_resend(void)
{
    if(master_request == MASTER_REQUEST_COMMAND)
    {
        master_send_command(master_command_value);
    }
    else if(master_request == MASTER_REQUEST_CLEAR_FAULT)
    {
        master_send_clear_fault();
    }
    else if(master_request == MASTER_REQUEST_RESET_CPLD)
    {
        master_send_reset_cpld();
    }
    else
    {
        master_send_read();
    }
}
/*
 * 解析响应帧
 */
static void master_handle_frame(const Uint8 *frame, Uint16 length)
{
    Uint16 reg[14];
    Uint16 index;
    //首个帧字节不是地址或者CRC错误
    if((frame[0] != MODBUS_CPLD_ADDRESS) ||  (modbus_frame_valid(frame, length) == 0))
    {
        modbus_cpld_stats.crc_errors++;
        return;
    }
    modbus_cpld_stats.rx_frames++;
    //命令字有效
    if((frame[1] & 0x80) != 0)
    {
        modbus_cpld_stats.exceptions++;
        if(master_request == MASTER_REQUEST_CLEAR_FAULT)
        {
            cpld_link_command.clear_fault_request = 0;
        }
        if(master_request == MASTER_REQUEST_RESET_CPLD)
        {
            cpld_link_command.reset_request = 0;
        }
        master_waiting = 0;
        master_next_poll_ms = system_millis + CPLD_POLL_PERIOD_MS;
        return;
    }
    //发的请求指令是04 解析数据
    if((master_request == MASTER_REQUEST_READ) &&  (frame[1] == 0x04) && (length == 33) && (frame[2] == 28))
    {
        for(index = 0; index < 14; index++)
        {
            reg[index] = get_be16(&frame[3 + (index * 2)]);
        }
        cpld_link_status.heartbeat = reg[1];
        cpld_link_status.vdc_raw = reg[2];
        cpld_link_status.vdc_average = reg[3];
        cpld_link_status.temperature_count = reg[4];
        cpld_link_status.state = reg[5];
        cpld_link_status.fault_bits = ((Uint32)reg[6] << 16) | reg[7];
        cpld_link_status.command_echo = reg[8];
        cpld_link_status.remote_valid_frames = reg[9];
        cpld_link_status.remote_error_frames = reg[10];
        cpld_link_status.remote_uart_errors = reg[11];
        cpld_link_status.remote_crc_errors = reg[12];
        cpld_link_status.remote_incomplete_frames = reg[13];
        cpld_link_status.valid = 1;
        cpld_link_status.link_flags = 0x0001;
        cpld_link_status.age_ticks = 0;
        cpld_last_valid_ms = system_millis;
        master_waiting = 0;
        master_retries = 0;
        master_next_poll_ms = system_millis + CPLD_POLL_PERIOD_MS;
    }
    //发的指令是06
    else if((master_request == MASTER_REQUEST_COMMAND) &&
            (frame[1] == 0x06) && (length == 8) &&
            (get_be16(&frame[2]) == 0x0100) &&
            (get_be16(&frame[4]) == master_command_value))
    {
        cpld_link_status.command_echo = master_command_value;
        cpld_last_valid_ms = system_millis;
        master_waiting = 0;
        master_retries = 0;
        master_next_poll_ms = system_millis + CPLD_POLL_PERIOD_MS;
    }
    else if((master_request == MASTER_REQUEST_CLEAR_FAULT) &&
            (frame[1] == 0x06) && (length == 8) &&
            (get_be16(&frame[2]) == 0x0101) &&
            (get_be16(&frame[4]) == MODBUS_CLEAR_FAULT_KEY))
    {
        cpld_link_command.clear_fault_request = 0;
        cpld_last_valid_ms = system_millis;
        master_waiting = 0;
        master_retries = 0;
        master_next_poll_ms = system_millis + CPLD_POLL_PERIOD_MS;
    }
    else if((master_request == MASTER_REQUEST_RESET_CPLD) &&
            (frame[1] == 0x06) && (length == 8) &&
            (get_be16(&frame[2]) == 0x0102) &&
            (get_be16(&frame[4]) == MODBUS_RESET_CPLD_KEY))
    {
        cpld_link_command.reset_request = 0;
        cpld_link_status.valid = 0;
        cpld_link_status.link_flags = 0x0002;
        cpld_last_valid_ms = system_millis;
        master_waiting = 0;
        master_retries = 0;
        master_next_poll_ms = system_millis + CPLD_POLL_PERIOD_MS;
    }
    else
    {
        modbus_cpld_stats.format_errors++;
    }
}
/*
 *  Modbus主站状态机函数
 */
static void master_service(void)
{
    Uint16 requested = (cpld_link_command.run_enable != 0) ? 1 : 0;

    if(master_waiting != 0)
    {
        //是否超时
        if((system_millis - master_sent_ms) >= CPLD_RESPONSE_TIMEOUT_MS)
        {
            modbus_cpld_stats.timeouts++;
            //重发一次
            if(master_retries < CPLD_MAX_RETRIES)
            {
                master_retries++;
                master_resend();
            }
            else
            {
                if(master_request == MASTER_REQUEST_CLEAR_FAULT)
                {
                    cpld_link_command.clear_fault_request = 0;
                }
                if(master_request == MASTER_REQUEST_RESET_CPLD)
                {
                    cpld_link_command.reset_request = 0;
                }
                master_waiting = 0;
                master_retries = 0;
                master_next_poll_ms = system_millis + CPLD_POLL_PERIOD_MS;
            }
        }
        return;
    }
    //超时
    if((system_millis - master_next_poll_ms) < 0x80000000)
    {
        //判断命令与回读命令是否一致
        if(requested != cpld_link_status.command_echo)
        {
            master_send_command(requested);
        }
        else if(cpld_link_command.clear_fault_request != 0)
        {
            master_send_clear_fault();
        }
        else if(cpld_link_command.reset_request != 0)
        {
            master_send_reset_cpld();
        }
        else
        {
            master_send_read();
        }
    }
}
/*
 * 寄存器地址与数据对应函数
 */
static Uint16 dsp_input_register(Uint16 address, Uint16 *value)
{
    float scaled;
    long signed_value;

    switch(address)
    {
    case 0x0000: *value = MODBUS_PROTOCOL_VERSION; break;
    case 0x0001: *value = (Uint16)(system_millis >> 16); break;
    case 0x0002: *value = (Uint16)system_millis; break;
    case 0x0003: *value = (Uint16)(adc_sample_count >> 16); break;
    case 0x0004: *value = (Uint16)adc_sample_count; break;
    case 0x0005: *value = adc_sample.vdc_raw; break;
    case 0x0006: *value = adc_sample.iac_raw; break;
    case 0x0007:
        scaled = adc_value.iac_a * 100.0;
        if(scaled > 32767.0) scaled = 32767.0;
        if(scaled < -32768.0) scaled = -32768.0;
        signed_value = (long)scaled;
        *value = (Uint16)signed_value;
        break;
    case 0x0008: *value = cpld_link_status.vdc_raw; break;
    case 0x0009: *value = cpld_link_status.vdc_average; break;
    case 0x000A: *value = cpld_link_status.temperature_count; break;
    case 0x000B: *value = cpld_link_status.state; break;
    case 0x000C: *value = (Uint16)(cpld_link_status.fault_bits >> 16); break;
    case 0x000D: *value = (Uint16)cpld_link_status.fault_bits; break;
    case 0x000E: *value = cpld_link_status.link_flags; break;
    case 0x000F: *value = g_gridPllLocked; break;
    case 0x0010:
        scaled = g_gridPllFrequencyHz * 100.0;
        if(scaled < 0.0) scaled = 0.0;
        if(scaled > 65535.0) scaled = 65535.0;
        *value = (Uint16)scaled;
        break;
    case 0x0011: *value = g_gridPllSignalValid; break;
    case 0x0012: *value = (Uint16)statcom_runtime.state; break;
    case 0x0013: *value = adc_zero_calibration.state; break;
    case 0x0014:
        *value = (Uint16)(modbus_cpld_stats.crc_errors +
                          modbus_cpld_stats.format_errors +
                          modbus_cpld_stats.timeouts +
                          modbus_cpld_stats.exceptions +
                          modbus_cpld_stats.overflows);
        break;
    case 0x0015:
        *value = (Uint16)(modbus_pc_stats.crc_errors +
                          modbus_pc_stats.format_errors +
                          modbus_pc_stats.exceptions +
                          modbus_pc_stats.overflows);
        break;
    case 0x0016: *value = cpld_link_status.command_echo; break;
    case 0x0017: *value = cpld_link_status.remote_uart_errors; break;
    case 0x0018: *value = cpld_link_status.remote_crc_errors; break;
    case 0x0019: *value = cpld_link_status.remote_incomplete_frames; break;
    case 0x001A: *value = (Uint16)modbus_cpld_stats.format_errors; break;
    case 0x001B: *value = (Uint16)modbus_cpld_stats.overflows; break;
    case 0x001C: *value = (Uint16)modbus_cpld_stats.timeouts; break;
    default: return 0;
    }
    return 1;
}
/*
 * 读保持器
 */
static Uint16 dsp_holding_register(Uint16 address, Uint16 *value)
{
    if(address == 0x0100)
    {
        *value = (cpld_link_command.run_enable != 0) ? 1 : 0;
        return 1;
    }
    if(address == 0x0101)
    {
        *value = (cpld_link_command.clear_fault_request != 0) ? 1 : 0;
        return 1;
    }
    if(address == 0x0102)
    {
        *value = (cpld_link_command.reset_request != 0) ? 1 : 0;
        return 1;
    }
    if(address == 0x0103)
    {
        *value = (dsp_reset_pending != 0) ? 1 : 0;
        return 1;
    }
    if(address == 0x0104)
    {
        *value = adc_zero_calibration.state;
        return 1;
    }
    return 0;
}
/*
 * 写保持器 检查数据
 * 输出0——地址不存在
 * 输出1——写入成功
 * 输出2——数值不合法
 */
static Uint16 dsp_write_holding(Uint16 address, Uint16 value)
{
    if(address == 0x0100)
    {
        if(value > 1)
        {
            return 2;
        }
        if((value != 0) &&
           ((cpld_link_command.clear_fault_request != 0) ||
            (cpld_link_command.reset_request != 0) ||
            (adc_zero_calibration.state == ADC_CALIBRATION_RUNNING)))
        {
            return 2;
        }
        cpld_link_command.run_enable = value;
        return 1;
    }
    if(address == 0x0101)
    {
        if((value != MODBUS_CLEAR_FAULT_KEY) ||
           (cpld_link_command.run_enable != 0) ||
           (cpld_link_command.reset_request != 0))
        {
            return 2;
        }
        cpld_link_command.clear_fault_request = 1;
        return 1;
    }
    if(address == 0x0102)
    {
        if((value != MODBUS_RESET_CPLD_KEY) ||
           (cpld_link_command.run_enable != 0) ||
           (cpld_link_command.clear_fault_request != 0) ||
           (cpld_link_command.reset_request != 0))
        {
            return 2;
        }
        cpld_link_command.reset_request = 1;
        return 1;
    }
    if(address == 0x0103)
    {
        if((value != MODBUS_RESET_DSP_KEY) ||
           (cpld_link_command.run_enable != 0) ||
           (dsp_reset_pending != 0))
        {
            return 2;
        }
        dsp_reset_pending = 1;
        return 1;
    }
    if(address == 0x0104)
    {
        if((value != MODBUS_ADC_ZERO_CAL_KEY) ||
           (cpld_link_command.run_enable != 0) ||
           (cpld_link_command.clear_fault_request != 0) ||
           (cpld_link_command.reset_request != 0) ||
           (adc_zero_calibration.state == ADC_CALIBRATION_RUNNING))
        {
            return 2;
        }
        adc_zero_calibration_start();
        return 1;
    }
    return 0;
}

/* SCI-B has already waited for TXEMPTY before this function is called. */
static void dsp_software_reset(void)
{
    DINT;
    EALLOW;
    SysCtrlRegs.WDCR = 0x0028;
    EDIS;
    for(;;)
    {
    }
}
/*
 * 地址或者数值非法需要发挥异常响应码函数
 * eg:功能码由06变成86
 */
static void slave_exception(Uint8 function, Uint8 exception)
{
    Uint8 response[5];
    response[0] = MODBUS_DSP_ADDRESS;   //地址
    response[1] = function | 0x80;  //异常功能码
    response[2] = exception;    //寄存器地址
    append_crc(response, 3);    //计算CRC
    sci_write_b(response, 5);
    modbus_pc_stats.exceptions++;
}

static void slave_handle_frame(const Uint8 *frame, Uint16 length) // 处理上位机发给DSP从站2的一帧Modbus RTU请求
{
    Uint8 response[MODBUS_FRAME_MAX]; // 保存准备通过SCI-B返回给上位机的响应帧
    Uint16 function;                  // 保存请求帧中的Modbus功能码
    Uint16 address;                   // 保存请求访问的起始寄存器地址
    Uint16 quantity;                  // 保存请求读写的寄存器数量
    Uint16 value;                     // 保存读出的寄存器值或准备写入的数值
    Uint16 index;                     // 循环处理寄存器或复制帧数据时使用
    Uint16 result;                    // 保存寄存器访问结果：0地址非法，1成功，2数值非法
    Uint16 response_length;           // 保存尚未加入2字节CRC时的响应长度

    if(frame[0] != MODBUS_DSP_ADDRESS) // 检查第0字节是否为DSP的Modbus从站地址2
    {
        return; // 地址不匹配时保持静默，不返回任何响应
    }

    if(modbus_frame_valid(frame, length) == 0) // 检查帧长度并校验Modbus CRC16
    {
        modbus_pc_stats.crc_errors++; // CRC或帧格式错误计数加1
        return;                       // 丢弃错误帧，不能继续解析其中的数据
    }

    modbus_pc_stats.rx_frames++; // 地址和CRC正确，累计一帧有效的上位机请求
    function = frame[1];         // 第1字节是Modbus功能码

    if((function == 0x03) || (function == 0x04)) // FC03读保持寄存器，FC04读输入寄存器
    {
        if(length != 8) // 标准FC03和FC04请求帧长度必须为8字节
        {
            slave_exception((Uint8)function, 0x03); // 长度错误，返回异常码03
            return;                                 // 异常响应发送后结束本次处理
        }

        address = get_be16(&frame[2]);  // 第2、3字节组成16位起始寄存器地址
        quantity = get_be16(&frame[4]); // 第4、5字节组成16位寄存器数量

        if((quantity == 0) || (quantity > 29)) // 限制数量，确保响应帧不超过64字节
        {
            slave_exception((Uint8)function, 0x03); // 数量为0或过大，返回异常码03
            return;                                 // 异常响应发送后结束本次处理
        }

        response[0] = MODBUS_DSP_ADDRESS;    // 响应第0字节填写DSP从站地址2
        response[1] = (Uint8)function;       // 正常响应回显原请求功能码
        response[2] = (Uint8)(quantity * 2); // 每个16位寄存器占2字节

        for(index = 0; index < quantity; index++) // 从起始地址开始逐个读取寄存器
        {
            result = (function == 0x04) ?
                dsp_input_register(address + index, &value) :
                dsp_holding_register(address + index, &value); // FC04读输入寄存器，FC03读保持寄存器

            if(result == 0) // 返回0表示请求的寄存器地址没有实现
            {
                slave_exception((Uint8)function, 0x02); // 地址不存在，返回异常码02
                return;                                 // 异常响应发送后结束本次处理
            }

            put_be16(&response[3 + (index * 2)], value); // 按高字节在前写入响应数据区
        }

        response_length = 3 + (quantity * 2);       // 计算地址、功能码、字节数和数据区总长度
        append_crc(response, response_length);      // 计算CRC并按低字节在前追加到帧尾
        sci_write_b(response, response_length + 2); // 通过SCI-B发送包含CRC的完整响应
        return;                                     // FC03或FC04请求处理完成
    }

    if(function == 0x06) // FC06写单个保持寄存器
    {
        if(length != 8) // 标准FC06请求帧长度必须为8字节
        {
            slave_exception(0x06, 0x03); // 长度错误，返回异常码03
            return;                      // 异常响应发送后结束本次处理
        }

        address = get_be16(&frame[2]);              // 第2、3字节组成要写入的保持寄存器地址
        value = get_be16(&frame[4]);                // 第4、5字节组成要写入的16位数值
        result = dsp_write_holding(address, value); // 检查地址和值，合法时执行写入

        if(result == 0) // 返回0表示保持寄存器地址不存在
        {
            slave_exception(0x06, 0x02); // 地址非法，返回异常码02
            return;                      // 异常响应发送后结束本次处理
        }

        if(result == 2) // 返回2表示准备写入的数值不合法
        {
            slave_exception(0x06, 0x03); // 数值非法，返回异常码03
            return;                      // 异常响应发送后结束本次处理
        }

        for(index = 0; index < 8; index++) // FC06正常响应需要原样回显8字节请求帧
        {
            response[index] = frame[index]; // 逐字节复制包含原CRC的请求帧
        }

        sci_write_b(response, 8); // 通过SCI-B发送FC06原样回显响应
        return;                   // FC06请求处理完成
    }

    if(function == 0x10) // FC10写多个保持寄存器
    {
        if(length != 11) // 当前仅支持写1个寄存器，因此请求总长度固定为11字节
        {
            slave_exception(0x10, 0x03); // 长度不符合当前格式，返回异常码03
            return;                      // 异常响应发送后结束本次处理
        }

        address = get_be16(&frame[2]);  // 第2、3字节组成起始保持寄存器地址
        quantity = get_be16(&frame[4]); // 第4、5字节组成写入寄存器数量

        if(address != 0x0100) // 清故障0101是一次性安全动作，只允许FC06写入
        {
            slave_exception(0x10, 0x02);
            return;
        }

        if((quantity != 1) || (frame[6] != 2)) // 当前只允许数量1且数据区字节数为2
        {
            slave_exception(0x10, 0x03); // 数量或字节数不合法，返回异常码03
            return;                      // 异常响应发送后结束本次处理
        }

        value = get_be16(&frame[7]);                // 第7、8字节组成要写入的16位数值
        result = dsp_write_holding(address, value); // 检查地址和值，合法时执行写入

        if(result == 0) // 返回0表示保持寄存器地址不存在
        {
            slave_exception(0x10, 0x02); // 地址非法，返回异常码02
            return;                      // 异常响应发送后结束本次处理
        }

        if(result == 2) // 返回2表示准备写入的数值不合法
        {
            slave_exception(0x10, 0x03); // 数值非法，返回异常码03
            return;                      // 异常响应发送后结束本次处理
        }

        response[0] = MODBUS_DSP_ADDRESS; // 正常响应第0字节填写DSP从站地址
        response[1] = 0x10;               // 正常响应第1字节填写FC10功能码
        put_be16(&response[2], address);   // 正常响应回显成功写入的起始地址
        put_be16(&response[4], quantity);  // 正常响应回显成功写入的寄存器数量
        append_crc(response, 6);           // 对前6字节计算CRC并追加到帧尾
        sci_write_b(response, 8);          // 通过SCI-B发送8字节FC10正常响应
        return;                            // FC10请求处理完成
    }

    slave_exception((Uint8)function, 0x01); // 其他功能码不支持，返回异常码01
}
void communication_task(void)
{
    Uint8 frame[MODBUS_FRAME_MAX];
    Uint16 length;
    Uint32 age;

    length = sci_take_frame(&sci_a_rx, frame);
    if(length != 0)
    {
        master_handle_frame(frame, length);
    }
    length = sci_take_frame(&sci_b_rx, frame);
    if(length != 0)
    {
        slave_handle_frame(frame, length);
    }

    age = system_millis - cpld_last_valid_ms;
    cpld_link_status.age_ticks = age;
    if(age >= CPLD_OFFLINE_TIMEOUT_MS)
    {
        cpld_link_status.valid = 0;
        cpld_link_status.link_flags = 0x0002;
    }
    else if(cpld_link_status.valid != 0)
    {
        cpld_link_status.link_flags = 0x0001;
    }
    if(modbus_cpld_stats.crc_errors != 0)
    {
        cpld_link_status.link_flags |= 0x0004;
    }
    if(modbus_cpld_stats.exceptions != 0)
    {
        cpld_link_status.link_flags |= 0x0008;
    }
    master_service();
    if(dsp_reset_pending != 0)
    {
        dsp_software_reset();
    }
}
