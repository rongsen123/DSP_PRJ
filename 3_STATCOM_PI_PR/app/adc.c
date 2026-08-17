#include "DSP28x_Project.h"
#include "adc.h"
#include "grid_pll.h"
#include "statcom_config.h"

volatile ADC_SAMPLE adc_sample = {0, 0, 0};
volatile ADC_ANALOG_VALUE adc_value = {0.0f, 0.0f, 0.0f,
                                      0.0f, 0.0f, 0.0f};
volatile ADC_ZERO_CALIBRATION adc_zero_calibration =
{
    2048.0f,
    2048.0f,
    0U,
    ADC_CALIBRATION_IDLE
};
volatile Uint16 adc_data_ready = 0;
volatile Uint32 adc_sample_count = 0;
volatile Uint32 adc_overflow_count = 0;
volatile Uint32 adc_queue_overflow_count = 0;
volatile Uint32 adc_isr_last_cycles = 0UL;
volatile Uint32 adc_isr_max_cycles = 0UL;
volatile float grid_halfwave_mean_counts = 0.0F;
volatile float grid_halfwave_centered_pu = 0.0F;
volatile Uint16 grid_halfwave_signal_valid = 0U;
volatile Uint16 grid_halfwave_clipped = 0U;

static Uint32 idc_zero_sum = 0UL;
static Uint32 iac_zero_sum = 0UL;
static Uint16 grid_dc_window[GRID_DC_WINDOW_SAMPLES];
static Uint32 grid_dc_sum = 0UL;
static Uint16 grid_dc_index = 0U;
static Uint16 grid_dc_count = 0U;
static Uint16 grid_samples_since_positive = GRID_SIGNAL_LOSS_SAMPLES;
static Uint16 grid_clip_count = 0U;
static volatile ADC_SAMPLE adc_process_queue[ADC_PROCESS_QUEUE_SIZE];
static volatile Uint16 adc_queue_head = 0U;
static volatile Uint16 adc_queue_tail = 0U;

static void adc_soc_init(void);
static void epwm1_adc_trigger_init(void);
static void adc_update_zero_calibration(const ADC_SAMPLE *sample);
static void adc_convert_to_analog(const ADC_SAMPLE *sample);
static void grid_pll_process_sample(Uint16 grid_raw);

void adc_init(void)
{
    Uint16 index;

    /* Load factory calibration, power the ADC and wait for it to settle. */
    InitAdc();

    /* ADCINA2 shares the AIO2 pin, so select its analog function. */
    InitAdcAio();

    adc_soc_init();
    epwm1_adc_trigger_init();

    for(index = 0U; index < GRID_DC_WINDOW_SAMPLES; index++)
    {
        grid_dc_window[index] = 0U;
    }
    GridPll_Init();

    EALLOW;
    PieVectTable.ADCINT1 = &AdcInt1Isr;
    EDIS;

    /* ADCINT1 is PIE group 1, channel 1. */
    PieCtrlRegs.PIEIER1.bit.INTx1 = 1;
    IER |= M_INT1;

    /* Perform one current-sensor zero calibration after sampling starts. */
    adc_zero_calibration_start();
}

static void adc_soc_init(void)
{
    EALLOW;

    /* REF2030 drives VREFHI with 3.0 V and VREFLO is tied to AGND. */
    AdcRegs.ADCCTL1.bit.ADCREFSEL = 1;

    /* Generate ADCINT1 only after the conversion result is latched. */
    AdcRegs.ADCCTL1.bit.INTPULSEPOS = 1;

    /* Sequential sampling; give SOC0-SOC2 fixed high priority. */
    AdcRegs.ADCSAMPLEMODE.all = 0x0000;
    AdcRegs.SOCPRICTL.bit.SOCPRIORITY = 3;

    /* SOC0: EPWM1 SOCA -> ADCINA0/VDC1 grid half-wave -> ADCRESULT0. */
    AdcRegs.ADCSOC0CTL.bit.CHSEL = 0;
    AdcRegs.ADCSOC0CTL.bit.TRIGSEL = 5;
    AdcRegs.ADCSOC0CTL.bit.ACQPS = 14;

    /* SOC1: EPWM1 SOCA -> ADCINA1/IDC1 -> ADCRESULT1. */
    AdcRegs.ADCSOC1CTL.bit.CHSEL = 1;
    AdcRegs.ADCSOC1CTL.bit.TRIGSEL = 5;
    AdcRegs.ADCSOC1CTL.bit.ACQPS = 14;

    /* SOC2: EPWM1 SOCA -> ADCINA2/IAC1 -> ADCRESULT2. */
    AdcRegs.ADCSOC2CTL.bit.CHSEL = 2;
    AdcRegs.ADCSOC2CTL.bit.TRIGSEL = 5;
    AdcRegs.ADCSOC2CTL.bit.ACQPS = 14;

    /* EOC2 is last, so it indicates that all three results are valid. */
    AdcRegs.INTSEL1N2.bit.INT1SEL = 2;
    AdcRegs.INTSEL1N2.bit.INT1CONT = 0;
    AdcRegs.INTSEL1N2.bit.INT1E = 1;

    /* ADC interrupts do not start additional SOCs in this application. */
    AdcRegs.ADCINTSOCSEL1.all = 0x0000;
    AdcRegs.ADCINTSOCSEL2.all = 0x0000;

    AdcRegs.ADCINTFLGCLR.bit.ADCINT1 = 1;
    AdcRegs.ADCINTOVFCLR.bit.ADCINT1 = 1;

    EDIS;
}

static void epwm1_adc_trigger_init(void)
{
    EALLOW;
    SysCtrlRegs.PCLKCR1.bit.EPWM1ENCLK = 1;
    EDIS;

    /* Stop only EPWM1 while changing its time-base and event registers. */
    EPwm1Regs.ETSEL.bit.SOCAEN = 0;
    EPwm1Regs.TBCTL.bit.CTRMODE = TB_FREEZE;

    EPwm1Regs.TBCTR = 0;
    EPwm1Regs.TBPHS.half.TBPHS = 0;
    EPwm1Regs.TBPRD = (Uint16)ADC_EPWM_TBPRD;

    EPwm1Regs.TBCTL.bit.PHSEN = TB_DISABLE;
    EPwm1Regs.TBCTL.bit.HSPCLKDIV = TB_DIV1;
    EPwm1Regs.TBCTL.bit.CLKDIV = TB_DIV1;

    /* One SOCA pulse at counter zero on every EPWM1 period. */
    EPwm1Regs.ETSEL.bit.SOCASEL = ET_CTR_ZERO;
    EPwm1Regs.ETPS.bit.SOCAPRD = ET_1ST;
    EPwm1Regs.ETCLR.bit.SOCA = 1U;

    /* EPWM1 directly triggers the ADC; no EPWM CPU interrupt is required. */
    EPwm1Regs.ETSEL.bit.INTEN = 0;
    EPwm1Regs.TBCTL.bit.CTRMODE = TB_COUNT_UP;
}

void adc_start(void)
{
    EALLOW;
    AdcRegs.ADCINTFLGCLR.bit.ADCINT1 = 1;
    AdcRegs.ADCINTOVFCLR.bit.ADCINT1 = 1;
    EDIS;

    EPwm1Regs.ETCLR.bit.SOCA = 1;
    EPwm1Regs.ETSEL.bit.SOCAEN = 1;
}

void adc_stop(void)
{
    EPwm1Regs.ETSEL.bit.SOCAEN = 0;
}

void adc_process(void)
{
    ADC_SAMPLE sample;
    Uint16 tail;

    if(adc_queue_head == adc_queue_tail)
    {
        adc_data_ready = 0U;
        return;
    }

    /* 每次主循环处理一个完整样本；SCI中断可在后续浮点计算期间抢占。 */
    DINT;
    tail = adc_queue_tail;
    sample.vdc_raw = adc_process_queue[tail].vdc_raw;
    sample.idc_raw = adc_process_queue[tail].idc_raw;
    sample.iac_raw = adc_process_queue[tail].iac_raw;
    adc_queue_tail = (tail + 1U) & ADC_PROCESS_QUEUE_MASK;
    adc_data_ready = (adc_queue_tail != adc_queue_head) ? 1U : 0U;
    EINT;

    /* 浮点和PLL全部在可被SCI抢占的前台执行，并保持逐样本顺序。 */
    adc_update_zero_calibration(&sample);
    adc_convert_to_analog(&sample);
    grid_pll_process_sample(sample.vdc_raw);
}

void adc_zero_calibration_start(void)
{
    idc_zero_sum = 0UL;
    iac_zero_sum = 0UL;
    adc_zero_calibration.sample_count = 0U;
    adc_zero_calibration.state = ADC_CALIBRATION_RUNNING;
}

static void adc_update_zero_calibration(const ADC_SAMPLE *sample)
{
    if(adc_zero_calibration.state != ADC_CALIBRATION_RUNNING)
    {
        return;
    }

    idc_zero_sum += (Uint32)sample->idc_raw;
    iac_zero_sum += (Uint32)sample->iac_raw;
    adc_zero_calibration.sample_count++;

    if(adc_zero_calibration.sample_count >= ADC_ZERO_CALIBRATION_SAMPLES)
    {
        adc_zero_calibration.idc_offset_count =
            (float)idc_zero_sum / (float)ADC_ZERO_CALIBRATION_SAMPLES;
        adc_zero_calibration.iac_offset_count =
            (float)iac_zero_sum / (float)ADC_ZERO_CALIBRATION_SAMPLES;
        adc_zero_calibration.state = ADC_CALIBRATION_DONE;
    }
}

static void adc_convert_to_analog(const ADC_SAMPLE *sample)
{
    /* ADC pin voltages, useful for checking the analog front end in CCS. */
    adc_value.vdc_pin_v = (float)sample->vdc_raw * ADC_VOLTS_PER_COUNT;
    adc_value.idc_pin_v = (float)sample->idc_raw * ADC_VOLTS_PER_COUNT;
    adc_value.iac_pin_v = (float)sample->iac_raw * ADC_VOLTS_PER_COUNT;

    /*
     * VDC1 is retained with the legacy engineering scale for bring-up.
     * In project 3 it is the isolated grid positive-half-wave input used by
     * the future PLL, not the actual DC-link feedback.  The real DC-link
     * voltage must arrive from the CPLD optical link.
     */
    adc_value.vdc_bus_v =
        (float)sample->vdc_raw * ADC_VDC_VOLTS_PER_COUNT;

    /* Bipolar current channels are centered near 1.5 V (about count 2048). */
    adc_value.idc_a =
        ((float)sample->idc_raw - adc_zero_calibration.idc_offset_count) *
        ADC_CURRENT_AMPS_PER_COUNT;
    adc_value.iac_a =
        ((float)sample->iac_raw - adc_zero_calibration.iac_offset_count) *
        ADC_CURRENT_AMPS_PER_COUNT;
}

static void grid_pll_process_sample(Uint16 grid_raw)
{
    Uint16 old_raw;
    float mean_counts;
    float centered_pu;

    if(grid_dc_count < GRID_DC_WINDOW_SAMPLES)
    {
        grid_dc_window[grid_dc_index] = grid_raw;
        grid_dc_sum += (Uint32)grid_raw;
        grid_dc_count++;
    }
    else
    {
        old_raw = grid_dc_window[grid_dc_index];
        grid_dc_window[grid_dc_index] = grid_raw;
        grid_dc_sum = grid_dc_sum - (Uint32)old_raw + (Uint32)grid_raw;
    }
    grid_dc_index++;
    if(grid_dc_index >= GRID_DC_WINDOW_SAMPLES)
    {
        grid_dc_index = 0U;
    }

    mean_counts = (float)grid_dc_sum / (float)grid_dc_count;
    centered_pu = (2.0F * ((float)grid_raw - mean_counts) *
                   GRID_INPUT_FULL_SCALE_VOLTS) /
                  (GRID_ADC_FULL_SCALE_COUNTS * GRID_NOMINAL_PEAK_VOLTS);

    if(grid_raw >= GRID_SIGNAL_PRESENT_COUNTS)
    {
        grid_samples_since_positive = 0U;
    }
    else if(grid_samples_since_positive < GRID_SIGNAL_LOSS_SAMPLES)
    {
        grid_samples_since_positive++;
    }

    if(grid_raw >= GRID_ADC_CLIP_HIGH_COUNTS)
    {
        if(grid_clip_count < GRID_ADC_CLIP_CONFIRM_SAMPLES)
        {
            grid_clip_count++;
        }
    }
    else
    {
        grid_clip_count = 0U;
    }

    grid_halfwave_clipped =
        (grid_clip_count >= GRID_ADC_CLIP_CONFIRM_SAMPLES) ? 1U : 0U;
    grid_halfwave_signal_valid =
        ((grid_dc_count >= GRID_DC_WINDOW_SAMPLES) &&
         (grid_samples_since_positive < GRID_SIGNAL_LOSS_SAMPLES) &&
         (grid_halfwave_clipped == 0U)) ? 1U : 0U;
    grid_halfwave_mean_counts = mean_counts;
    grid_halfwave_centered_pu = centered_pu;
    GridPll_Run(centered_pu, grid_halfwave_signal_valid);
}

interrupt void AdcInt1Isr(void)
{
    Uint32 isr_start_cycles = CpuTimer1Regs.TIM.all;
    Uint16 next_head;
    ADC_SAMPLE sample;

    /* EOC2 has occurred, so ADCRESULT0-2 all belong to this sample set. */
    sample.vdc_raw = AdcResult.ADCRESULT0;
    sample.idc_raw = AdcResult.ADCRESULT1;
    sample.iac_raw = AdcResult.ADCRESULT2;
    adc_sample.vdc_raw = sample.vdc_raw;
    adc_sample.idc_raw = sample.idc_raw;
    adc_sample.iac_raw = sample.iac_raw;

    next_head = (adc_queue_head + 1U) & ADC_PROCESS_QUEUE_MASK;
    if(next_head != adc_queue_tail)
    {
        adc_process_queue[adc_queue_head].vdc_raw = sample.vdc_raw;
        adc_process_queue[adc_queue_head].idc_raw = sample.idc_raw;
        adc_process_queue[adc_queue_head].iac_raw = sample.iac_raw;
        adc_queue_head = next_head;
        adc_data_ready = 1U;
    }
    else
    {
        adc_queue_overflow_count++;
    }

    adc_sample_count++;

    AdcRegs.ADCINTFLGCLR.bit.ADCINT1 = 1;

    if(AdcRegs.ADCINTOVF.bit.ADCINT1 != 0)
    {
        adc_overflow_count++;
        AdcRegs.ADCINTOVFCLR.bit.ADCINT1 = 1;
        AdcRegs.ADCINTFLGCLR.bit.ADCINT1 = 1;
    }

    adc_isr_last_cycles = isr_start_cycles - CpuTimer1Regs.TIM.all;
    if(adc_isr_last_cycles > adc_isr_max_cycles)
    {
        adc_isr_max_cycles = adc_isr_last_cycles;
    }

    PieCtrlRegs.PIEACK.all = PIEACK_GROUP1;
}
