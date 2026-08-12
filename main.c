#include "DSP28x_Project.h"
#include "led.h"
#include "sci.h"
/**
 * main.c
 */
void main(void)
{
    InitSysCtrl();             // clock, watchdog and peripheral clocks

    DINT;

    InitPieCtrl();
    IER = 0x0000;
    IFR = 0x0000;
    InitPieVectTable();

    led_init();
    uart_init(230400);

    IER |= M_INT9;             // enable CPU INT9 (SCIA RX)
    EINT;
    ERTM;
    while(1)
    {
        if(frame_ready != 0U)
        {
            /* Copy the validated request and start the interrupt-driven echo. */
            uart_send_frame(&rx_frame);
        }

        if(tx_complete != 0U)
        {
            DINT;
            tx_complete = 0U;
            GpioDataRegs.GPATOGGLE.bit.GPIO10 = 1;
            EINT;
        }
    }

}
