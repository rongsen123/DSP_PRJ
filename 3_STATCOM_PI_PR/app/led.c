/*
 * led.c
 *
 *  Created on: 2026-08-05
 *      Author: 高荣森
 */
#include "DSP28x_Project.h"
/*
 * LED GPIO init
 * LED1: GPIO10
 * LED2: GPIO55
 * Default pull-up = off
 */
void led_init(void)
{
    EALLOW;

    GpioCtrlRegs.GPAMUX1.bit.GPIO10=0; // GPIO function
    GpioCtrlRegs.GPADIR.bit.GPIO10=1;  // output
    GpioCtrlRegs.GPAPUD.bit.GPIO10=0;  // enable pull-up

    GpioCtrlRegs.GPBMUX2.bit.GPIO55=0; // GPIO function
    GpioCtrlRegs.GPBDIR.bit.GPIO55=1;  // output
    GpioCtrlRegs.GPBPUD.bit.GPIO55=0;  // enable pull-up

    EDIS;

    GpioDataRegs.GPASET.bit.GPIO10=1;
    GpioDataRegs.GPBSET.bit.GPIO55=1;
}

/*
 * delay_ms
 */

void delay_ms(Uint16 num)
{
    Uint16 i;
    volatile Uint16 j;   // volatile prevents loop optimization
    for(i=0;i<num;i++)
    {
        for(j=0;j<1120;j++);
    }
}


