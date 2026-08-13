#include "DSP28x_Project.h"
#include "led.h"
#include "sci.h"
#include "adc.h"

volatile Uint16 system_startup_stage = 0U;
volatile Uint32 main_loop_count = 0UL;

static void copy_ramfuncs_to_ram(void)
{
    Uint16 *source = &RamfuncsLoadStart;
    Uint16 *destination = &RamfuncsRunStart;

    while(source < &RamfuncsLoadEnd)
    {
        *destination++ = *source++;
    }
}
/**
 * main.c
 */
void main(void)
{
    InitSysCtrl();             // clock, watchdog and peripheral clocks

    /*
     * DSP28x_usDelay is linked with a Flash load address and a RAM run
     * address.  InitAdc() calls DELAY_US(), so the ramfuncs section must be
     * copied before ADC initialization.
     */
    copy_ramfuncs_to_ram();
    system_startup_stage = 1U;

    DINT;

    InitPieCtrl();
    IER = 0x0000;
    IFR = 0x0000;
    InitPieVectTable();
    system_startup_stage = 2U;

    led_init();
    uart_init(230400);
    system_startup_stage = 3U;
    adc_init();
    system_startup_stage = 4U;

    IER |= M_INT9;             // enable CPU INT9 (SCIA RX)
    EINT;
    ERTM;
    system_startup_stage = 5U;

    /* Start 10 kHz EPWM1 SOCA hardware-triggered ADC sampling. */
    adc_start();
    system_startup_stage = 6U;

    while(1)
    {
        main_loop_count++;
        adc_process();

        if(frame_ready != 0U)
        {
            /* Copy the validated request and start the interrupt-driven echo. */
            uart_send_frame(&rx_frame);
        }

        if(tx_complete != 0U)
        {
            DINT;
            tx_complete = 0;
            GpioDataRegs.GPATOGGLE.bit.GPIO10 = 1;
            EINT;
        }
    }

}
