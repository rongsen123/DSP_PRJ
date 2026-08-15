#include "DSP28x_Project.h"
#include "led.h"
#include "sci.h"
#include "adc.h"
#include "statcom_runtime.h"

volatile Uint32 main_loop_count = 0;

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

    DINT;

    InitPieCtrl();
    IER = 0x0000;
    IFR = 0x0000;
    InitPieVectTable();

    led_init();
    communication_init();
    adc_init();
    statcom_runtime_init();

    EINT;
    ERTM;

    /* Start 20 kHz EPWM1 SOCA hardware-triggered ADC sampling. */
    adc_start();

    while(1)
    {
        main_loop_count++;
        adc_process();
        communication_task();
        statcom_runtime_step();
    }

}
