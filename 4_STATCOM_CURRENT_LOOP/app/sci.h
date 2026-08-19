#ifndef APP_SCI_H_
#define APP_SCI_H_

#include "DSP28x_Project.h"


/*
 * 基本参数
 */
#define MODBUS_CPLD_ADDRESS          1      //CPLD地址
#define MODBUS_DSP_ADDRESS           2      //DSP地址
#define MODBUS_SCIA_BAUD_RATE        115200UL //DSP主站与CPLD从站
#define MODBUS_SCIB_BAUD_RATE        115200UL //上位机主站与DSP从站
#define MODBUS_FRAME_MAX             64     //数据最大数量
#define MODBUS_PROTOCOL_VERSION      0x0105 // ADC遥测与三项保护阈值统一为原始码
#define MODBUS_CLEAR_FAULT_KEY       0xA55A //清除锁存故障写入密钥
#define MODBUS_RESET_CPLD_KEY        0xC33C //复位CPLD内部状态机写入密钥
#define MODBUS_RESET_DSP_KEY         0xD55D //通过看门狗复位DSP写入密钥
#define MODBUS_ADC_ZERO_CAL_KEY      0xCA1B //重新开始1024点电流零漂校准

/*
 * 状态结构体定义
 */
typedef struct
{
    Uint32 rx_frames;
    Uint32 tx_frames;
    Uint32 crc_errors;
    Uint32 format_errors;
    Uint32 timeouts;
    Uint32 exceptions;
    Uint32 overflows;
} MODBUS_LINK_STATS;

extern volatile Uint32 system_millis;
extern volatile MODBUS_LINK_STATS modbus_cpld_stats;
extern volatile MODBUS_LINK_STATS modbus_pc_stats;

void communication_init(void);
void communication_task(void);
Uint16 Modbus_Crc16(const Uint8 *data, Uint16 length);

#endif /* APP_SCI_H_ */
