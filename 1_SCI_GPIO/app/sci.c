/*
 * sci.c
 *
 *  Created on: 2026-08-05
 *      Author: 楂樿崳妫�
 */

#include "DSP28x_Project.h"
#include "sci.h"

volatile RX_STATE rx_state = WAIT_HEAD1;
volatile com_data_uion rx_frame;
volatile Uint8 index = 0;
volatile Uint8 rx_length = 0;
volatile Uint8 frame_ready = 0;

volatile com_data_uion tx_frame;
volatile Uint16 index_1 = 0;
volatile Uint8 tx_length = 0;
volatile Uint8 tx_busy = 0;
volatile Uint8 tx_complete = 0;


interrupt void SciRxIsr(void);
interrupt void SciTxIsr(void);

static void rx_parser_reset(void);
static Uint8 tx_frame_byte(Uint16 position);
static void uart_finish_transmit(void);

/*
 * UART init
 * SCITXA: GPIO12
 * SCIRXA: GPIO7
 * 串口初始化
 */
void uart_init(Uint32 baud)
{
    Uint32 scibaud;
    Uint16 scihbaud;
    Uint16 scilbaud;

    /* BRR = LSPCLK/(8*baud)-1.  Use 32-bit arithmetic on the C28x. */
    scibaud = ((SCI_LSPCLK_HZ + (baud * 4UL)) / (baud * 8UL)) - 1UL;
    scihbaud = (scibaud >> 8 ) & 0xFF;
    scilbaud = scibaud & 0xFF;

    EALLOW;
    SysCtrlRegs.PCLKCR0.bit.SCIAENCLK=1; // enable SCIA clock
    EDIS;

    InitSciGpio();

    /* Hold SCI in software reset while programming it: 8-N-1, idle-line. */
    SciaRegs.SCICTL1.all = 0x0003;
    SciaRegs.SCICCR.all = 0x0007;
    SciaRegs.SCICTL2.all = 0x0000;
    SciaRegs.SCICTL2.bit.RXBKINTENA = 1;
    SciaRegs.SCICTL2.bit.TXINTENA = 1;

    SciaRegs.SCIHBAUD=scihbaud;
    SciaRegs.SCILBAUD=scilbaud;

    /* RX interrupts immediately; TX FIFO interrupt is enabled per frame. */
    SciaRegs.SCIFFTX.all = 0xE040;
    SciaRegs.SCIFFRX.all = 0x2061;
    SciaRegs.SCIFFCT.all = 0x0000;

    SciaRegs.SCICTL1.all = 0x0023;
    SciaRegs.SCIFFTX.bit.TXFIFOXRESET = 1;
    SciaRegs.SCIFFRX.bit.RXFIFORESET = 1;

    EALLOW;
    PieVectTable.SCIRXINTA = &SciRxIsr;
    PieVectTable.SCITXINTA = &SciTxIsr;
    EDIS;

    PieCtrlRegs.PIEIER9.bit.INTx1 = 1;
    PieCtrlRegs.PIEIER9.bit.INTx2 = 0;
}

/*
 * UART receive buffer handler
 */
void uart_receive_byte(Uint8 rx_byte)
{
    /* Keep one outstanding request until its response has fully completed. */
    if(frame_ready != 0U)
    {
        return;
    }

    switch(rx_state)
    {
    case WAIT_HEAD1:
    {
        if(rx_byte == FRAME_HEADER_1)
            {
                rx_frame.headers_1=rx_byte;
                rx_state=WAIT_HEAD2;
            }
        else    rx_state=WAIT_HEAD1;
        break;
    }
    case WAIT_HEAD2:
    {
        if(rx_byte == FRAME_HEADER_2)
            {
                rx_frame.headers_2=rx_byte;
                rx_state=WAIT_LENGTH;
            }
        else if(rx_byte == FRAME_HEADER_1)
        {
            /* The current byte can also be the start of the next frame. */
            rx_frame.headers_1 = rx_byte;
            rx_state = WAIT_HEAD2;
        }
        else
        {
            rx_state = WAIT_HEAD1;
        }
        break;
    }
    case WAIT_LENGTH:
    {
        rx_frame.data_length=rx_byte;
        rx_length=rx_byte;
        index=0;

        if(rx_length == 0)
        {
            rx_state = RECEIVE_CRC;
        }
        else if(rx_length <= RX_MAX_NUM)
        {
            rx_state = RECEIVE_DATA;
        }
        else
        {
            rx_state = WAIT_HEAD1;
            index = 0;
            rx_length = 0;
        }


        break;
    }
    case RECEIVE_DATA:
    {

        rx_frame.data[index]=rx_byte;
        index++;

        if(index>=rx_length)
            {
                rx_state=RECEIVE_CRC;
                index=0;
            }

        break;
    }
    case RECEIVE_CRC:
    {
        rx_frame.data_crc=rx_byte;
        if(CRC_Check(&rx_frame))
        {
            frame_ready=1;
        }
        rx_state=WAIT_HEAD1;
        break;
    }
    default:
        rx_state=WAIT_HEAD1;
        break;
    }
}


Uint16 CRC_Check(const volatile com_data_uion *frame)
{
    Uint8 crc=0;
    Uint8 i;

    crc ^= frame->headers_1;
    crc ^= frame->headers_2;
    crc ^= frame->data_length;
    for(i=0;i<frame->data_length;i++)
    {
        crc ^= frame->data[i];
    }
    return (crc == frame->data_crc) ? 1U : 0U;
}

static void rx_parser_reset(void)
{
    rx_state = WAIT_HEAD1;
    index = 0U;
    rx_length = 0U;
}

static Uint8 tx_frame_byte(Uint16 position)
{
    if(position == 0U)
    {
        return tx_frame.headers_1;
    }
    if(position == 1U)
    {
        return tx_frame.headers_2;
    }
    if(position == 2U)
    {
        return tx_frame.data_length;
    }
    if(position < ((Uint16)tx_length + 3U))
    {
        return tx_frame.data[position - 3U];
    }

    return tx_frame.data_crc;
}

void uart_send_frame(const volatile com_data_uion *frame)
{
    Uint16 i;
    Uint16 length = frame->data_length;

    if((length > TX_MAX_NUM) || (tx_busy != 0U))
    {
        return;
    }

    /* Stop RX before copying the shared request and starting the reply. */
    PieCtrlRegs.PIEIER9.bit.INTx1 = 0;
    SciaRegs.SCIFFRX.bit.RXFFIENA = 0;

    tx_frame.headers_1 = frame->headers_1;
    tx_frame.headers_2 = frame->headers_2;
    tx_frame.data_length = frame->data_length;
    for(i = 0U; i < length; i++)
    {
        tx_frame.data[i] = frame->data[i];
    }
    tx_frame.data_crc = frame->data_crc;

    tx_length = (Uint8)length;
    index_1 = 0U;
    tx_complete = 0U;
    tx_busy = 1U;
    frame_ready = 0U;

    /* Start from an empty FIFO. Empty status generates the first TX IRQ. */
    SciaRegs.SCIFFTX.bit.TXFIFOXRESET = 0;
    SciaRegs.SCIFFTX.bit.TXFFINTCLR = 1;
    SciaRegs.SCIFFTX.bit.TXFIFOXRESET = 1;
    PieCtrlRegs.PIEACK.all = PIEACK_GROUP9;
    PieCtrlRegs.PIEIER9.bit.INTx2 = 1;
    SciaRegs.SCIFFTX.bit.TXFFIENA = 1;
}

interrupt void SciRxIsr(void)
{
    Uint16 rx_status;
    Uint16 rx_word;
    Uint16 rx_error = SciaRegs.SCIFFRX.bit.RXFFOVF;

    /* Drain the FIFO. RXBUF[15:14] contains per-byte FE/PE in FIFO mode. */
    while(SciaRegs.SCIFFRX.bit.RXFFST != 0U)
    {
        rx_status = SciaRegs.SCIRXST.all;
        rx_word = SciaRegs.SCIRXBUF.all;

        /* PE=bit2, OE=bit3, FE=bit4, BRKDT=bit5, RXERROR=bit7. */
        if(((rx_status & 0x00BCU) != 0U) ||
           ((rx_word & 0xC000U) != 0U))
        {
            rx_error = 1U;
            continue;
        }

        if(rx_error == 0U)
        {
            uart_receive_byte((Uint8)(rx_word & 0x00FFU));
        }
    }

    if(rx_error != 0U)
    {
        rx_parser_reset();

        /* SCI status error bits are read-only; SWRESET clears the receiver. */
        SciaRegs.SCICTL1.bit.SWRESET = 0;
        SciaRegs.SCIFFRX.bit.RXFIFORESET = 0;
        SciaRegs.SCIFFRX.bit.RXFIFORESET = 1;
        SciaRegs.SCICTL1.bit.SWRESET = 1;
    }

    SciaRegs.SCIFFRX.bit.RXFFOVRCLR = 1;
    SciaRegs.SCIFFRX.bit.RXFFINTCLR = 1;
    PieCtrlRegs.PIEACK.all = PIEACK_GROUP9;
}

interrupt void SciTxIsr(void)
{
    Uint16 total_length = (Uint16)tx_length + FRAME_OVERHEAD;

    /* Fill all currently free FIFO locations. */
    while((SciaRegs.SCIFFTX.bit.TXFFST < SCI_FIFO_DEPTH) &&
          (index_1 < total_length))
    {
        SciaRegs.SCITXBUF = (Uint16)tx_frame_byte(index_1);
        index_1++;
    }

    SciaRegs.SCIFFTX.bit.TXFFINTCLR = 1;

    if(index_1 >= total_length)
    {
        /*
         * With TXFFIL=0 this interrupt occurs again when the FIFO is empty.
         * One byte may still be in the shift register, so wait for TXEMPTY.
         */
        if(SciaRegs.SCIFFTX.bit.TXFFST == 0U)
        {
            while(SciaRegs.SCICTL2.bit.TXEMPTY == 0U)
            {
            }
            uart_finish_transmit();
        }
    }

    PieCtrlRegs.PIEACK.all = PIEACK_GROUP9;
}

static void uart_finish_transmit(void)
{
    SciaRegs.SCIFFTX.bit.TXFFIENA = 0;
    PieCtrlRegs.PIEIER9.bit.INTx2 = 0;
    SciaRegs.SCIFFTX.bit.TXFFINTCLR = 1;

    /* Discard local echo/bytes received during the half-duplex response. */
    SciaRegs.SCICTL1.bit.SWRESET = 0;
    SciaRegs.SCIFFRX.bit.RXFIFORESET = 0;
    SciaRegs.SCIFFRX.bit.RXFFOVRCLR = 1;
    SciaRegs.SCIFFRX.bit.RXFFINTCLR = 1;
    SciaRegs.SCIFFRX.bit.RXFIFORESET = 1;
    SciaRegs.SCICTL1.bit.SWRESET = 1;

    rx_parser_reset();
    tx_busy = 0U;
    tx_complete = 1U;

    SciaRegs.SCIFFRX.bit.RXFFIENA = 1;
    PieCtrlRegs.PIEIER9.bit.INTx1 = 1;
}

