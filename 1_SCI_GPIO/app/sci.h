/*
 * sci.h
 *
 *  Created on: 2026-08-05
 *      Author: 楂樿崳妫�
 */

#ifndef APP_SCI_H_
#define APP_SCI_H_

#include "DSP28x_Project.h"

#define RX_MAX_NUM       32U
#define TX_MAX_NUM       RX_MAX_NUM
#define FRAME_HEADER_1   0xAAU
#define FRAME_HEADER_2   0x55U
#define FRAME_OVERHEAD   4U
#define SCI_FIFO_DEPTH   16U
#define SCI_LSPCLK_HZ    22500000UL

typedef struct
{
    Uint8 headers_1;
    Uint8 headers_2;
    Uint8 data_length;
    Uint8 data[RX_MAX_NUM];
    Uint8 data_crc;
} com_data_uion;



typedef enum
{
    WAIT_HEAD1 = 0,
    WAIT_HEAD2,
    WAIT_LENGTH,
    RECEIVE_DATA,
    RECEIVE_CRC
} RX_STATE;

/* Global extern declarations for cross-file reference */
extern volatile RX_STATE rx_state;
extern volatile com_data_uion rx_frame;
extern volatile Uint8 index;
extern volatile Uint8 rx_length;
extern volatile Uint8 frame_ready;

extern volatile com_data_uion tx_frame;
extern volatile Uint16 index_1;
extern volatile Uint8 tx_length;
extern volatile Uint8 tx_busy;
extern volatile Uint8 tx_complete;


/* Function declarations */
void uart_init(Uint32 baud);
void uart_receive_byte(Uint8 rx_byte);
void uart_send_frame(const volatile com_data_uion *frame);
Uint16 CRC_Check(const volatile com_data_uion *frame);

#endif /* APP_SCI_H_ */
