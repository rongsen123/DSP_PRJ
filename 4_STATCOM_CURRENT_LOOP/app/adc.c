#include "DSP28x_Project.h"
#include "adc.h"
#include "grid_pll.h"
#include "statcom_config.h"
#include "protection.h"

volatile ADC_SAMPLE adc_sample = {0, 0};
volatile ADC_ZERO_CALIBRATION adc_zero_calibration =
{
    2048U,
    0U,
    ADC_CALIBRATION_IDLE
};
volatile Uint16 adc_data_ready = 0;
volatile Uint32 adc_sample_count = 0;
volatile Uint32 adc_processed_count = 0;
volatile Uint32 adc_pll_sample_count = 0;
volatile Uint32 adc_overflow_count = 0;
volatile Uint32 adc_queue_overflow_count = 0;
volatile Uint32 adc_isr_last_cycles = 0UL;
volatile Uint32 adc_isr_max_cycles = 0UL;
volatile Uint32 adc_fast_current_count = 0UL;
volatile int16 adc_fast_current_delta_count = 0;
volatile Uint16 adc_fast_current_abs_count = 0U;
volatile Uint16 adc_fast_current_peak_count = 0U;
volatile Uint16 adc_fast_overcurrent_active = 0U;
volatile Uint16 adc_fast_overcurrent_latched = 0U;
volatile Uint16 adc_fast_overcurrent_confirm_count = 0U;
volatile Uint16 adc_fast_grid_raw = 0U;
volatile Uint16 adc_fast_grid_peak_raw = 0U;
volatile Uint16 adc_fast_grid_overvoltage_active = 0U;
volatile Uint16 adc_fast_grid_overvoltage_latched = 0U;
volatile Uint16 adc_fast_grid_overvoltage_confirm_count = 0U;
volatile float grid_halfwave_mean_counts = 0.0F;
volatile float grid_halfwave_centered_pu = 0.0F;
volatile Uint16 grid_halfwave_signal_valid = 0U;
volatile Uint16 grid_halfwave_clipped = 0U;

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
static Uint16 adc_queue_decimation_count = 0U;

static void adc_soc_init(void);
static void epwm1_adc_trigger_init(void);
#if STATCOM_FAST_PROTECTION_DIAGNOSTIC_BYPASS == 0U
static void adc_fast_protection_process(Uint16 grid_raw, Uint16 iac_raw);
#else
static void adc_diagnostic_update_zero_calibration(const ADC_SAMPLE *sample);
#endif
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

    /* Sequential sampling; give the two active SOCs fixed high priority. */
    AdcRegs.ADCSAMPLEMODE.all = 0x0000;
    AdcRegs.SOCPRICTL.bit.SOCPRIORITY = 2;

    /* SOC0: EPWM1 SOCA -> ADCINA0/VDC1 grid half-wave -> ADCRESULT0. */
    AdcRegs.ADCSOC0CTL.bit.CHSEL = 0;
    AdcRegs.ADCSOC0CTL.bit.TRIGSEL = 5;
    AdcRegs.ADCSOC0CTL.bit.ACQPS = 14;

    /* SOC1: EPWM1 SOCA -> ADCINA2/IAC1 -> ADCRESULT1. */
    AdcRegs.ADCSOC1CTL.bit.CHSEL = 2;
    AdcRegs.ADCSOC1CTL.bit.TRIGSEL = 5;
    AdcRegs.ADCSOC1CTL.bit.ACQPS = 14;

    /* EOC1 is last, so it indicates that both conversion results are valid. */
    AdcRegs.INTSEL1N2.bit.INT1SEL = 1;
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
    Uint16 processed = 0U;

    while(processed < ADC_PROCESS_BATCH_LIMIT)
    {
        if(adc_queue_head == adc_queue_tail)
        {
            adc_data_ready = 0U;
            break;
        }

        /* 只在移动队列尾指针时短暂关中断，浮点处理仍可被SCI抢占。 */
        DINT;
        tail = adc_queue_tail;
        sample.vdc_raw = adc_process_queue[tail].vdc_raw;
        sample.iac_raw = adc_process_queue[tail].iac_raw;
        adc_queue_tail = (tail + 1U) & ADC_PROCESS_QUEUE_MASK;
        adc_data_ready = (adc_queue_tail != adc_queue_head) ? 1U : 0U;
        EINT;

#if STATCOM_FAST_PROTECTION_DIAGNOSTIC_BYPASS != 0U
        adc_diagnostic_update_zero_calibration(&sample);
#endif
        adc_processed_count++;

        /* ISR已经完成20 kHz到4 kHz的确定性抽取，队列中的每点都运行PLL。 */
        grid_pll_process_sample(sample.vdc_raw);
        adc_pll_sample_count++;
        processed++;
    }
}

void adc_zero_calibration_start(void)
{
    iac_zero_sum = 0UL;
    adc_zero_calibration.sample_count = 0U;
    adc_zero_calibration.state = ADC_CALIBRATION_RUNNING;
}

#if STATCOM_FAST_PROTECTION_DIAGNOSTIC_BYPASS != 0U
static void adc_diagnostic_update_zero_calibration(const ADC_SAMPLE *sample)
{
    if(adc_zero_calibration.state != ADC_CALIBRATION_RUNNING)
    {
        return;
    }

    iac_zero_sum += (Uint32)sample->iac_raw;
    adc_zero_calibration.sample_count++;
    if(adc_zero_calibration.sample_count >= ADC_ZERO_CALIBRATION_SAMPLES)
    {
        adc_zero_calibration.iac_offset_count =
            (Uint16)(iac_zero_sum >> 10);
        adc_zero_calibration.state = ADC_CALIBRATION_DONE;
    }
}
#endif

#if STATCOM_FAST_PROTECTION_DIAGNOSTIC_BYPASS == 0U
static void adc_fast_protection_process(Uint16 grid_raw, Uint16 iac_raw)
{
    long current_delta;
    Uint16 current_abs_count;

    if(adc_zero_calibration.state == ADC_CALIBRATION_RUNNING)
    {
        iac_zero_sum += (Uint32)iac_raw;
        adc_zero_calibration.sample_count++;

        if(adc_zero_calibration.sample_count >= ADC_ZERO_CALIBRATION_SAMPLES)
        {
            adc_zero_calibration.iac_offset_count =
                (Uint16)(iac_zero_sum >> 10);
            adc_zero_calibration.state = ADC_CALIBRATION_DONE;
        }
    }

    current_delta = (long)iac_raw -
                    (long)adc_zero_calibration.iac_offset_count;
    current_abs_count = (Uint16)((current_delta >= 0L) ?
                                 current_delta : -current_delta);
    adc_fast_current_delta_count = (int16)current_delta;
    adc_fast_current_abs_count = current_abs_count;
    adc_fast_current_count++;

    if(current_abs_count > adc_fast_current_peak_count)
    {
        adc_fast_current_peak_count = current_abs_count;
    }

#if STATCOM_FAST_OVERCURRENT_MONITOR_ENABLED != 0U
    if((adc_zero_calibration.state == ADC_CALIBRATION_DONE) &&
       (current_abs_count >= protection_iac_trip_delta_count))
    {
        adc_fast_overcurrent_active = 1U;
        if(adc_fast_overcurrent_confirm_count <
           STATCOM_FAST_OVERCURRENT_CONFIRM_SAMPLES)
        {
            adc_fast_overcurrent_confirm_count++;
        }
        if(adc_fast_overcurrent_confirm_count >=
           STATCOM_FAST_OVERCURRENT_CONFIRM_SAMPLES)
        {
            adc_fast_overcurrent_latched = 1U;
        }
    }
    else
    {
        adc_fast_overcurrent_active = 0U;
        adc_fast_overcurrent_confirm_count = 0U;
    }
#else
    adc_fast_overcurrent_active = 0U;
    adc_fast_overcurrent_confirm_count = 0U;
#endif

    adc_fast_grid_raw = grid_raw;
    if(grid_raw > adc_fast_grid_peak_raw)
    {
        adc_fast_grid_peak_raw = grid_raw;
    }

    if(grid_raw >= protection_grid_peak_trip_raw)
    {
        adc_fast_grid_overvoltage_active = 1U;
        if(adc_fast_grid_overvoltage_confirm_count <
           STATCOM_FAST_GRID_OV_CONFIRM_SAMPLES)
        {
            adc_fast_grid_overvoltage_confirm_count++;
        }
        if(adc_fast_grid_overvoltage_confirm_count >=
           STATCOM_FAST_GRID_OV_CONFIRM_SAMPLES)
        {
            adc_fast_grid_overvoltage_latched = 1U;
        }
    }
    else
    {
        adc_fast_grid_overvoltage_active = 0U;
        adc_fast_grid_overvoltage_confirm_count = 0U;
    }
}
#endif

void adc_protection_clear_latched(void)
{
    adc_fast_overcurrent_latched = adc_fast_overcurrent_active;
    adc_fast_grid_overvoltage_latched = adc_fast_grid_overvoltage_active;
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

    /* EOC1 has occurred, so ADCRESULT0-1 belong to this two-channel set. */
    sample.vdc_raw = AdcResult.ADCRESULT0;
    sample.iac_raw = AdcResult.ADCRESULT1;
    adc_sample.vdc_raw = sample.vdc_raw;
    adc_sample.iac_raw = sample.iac_raw;

    /* Current conversion, zero calibration and protection diagnostics stay on
     * the strict 20 kHz ADC interrupt path.  Future PR shadow control attaches
     * here; it must not consume the 4 kHz PLL queue as current feedback. */
#if STATCOM_FAST_PROTECTION_DIAGNOSTIC_BYPASS == 0U
    adc_fast_protection_process(sample.vdc_raw, sample.iac_raw);
#endif

    /* 严格选取第5、10、15...次ADC结果，形成均匀的4 kHz处理时基。 */
    adc_queue_decimation_count++;
    if(adc_queue_decimation_count >= ADC_QUEUE_DECIMATION)
    {
        adc_queue_decimation_count = 0U;
        next_head = (adc_queue_head + 1U) & ADC_PROCESS_QUEUE_MASK;
        if(next_head != adc_queue_tail)
        {
            adc_process_queue[adc_queue_head].vdc_raw = sample.vdc_raw;
            adc_process_queue[adc_queue_head].iac_raw = sample.iac_raw;
            adc_queue_head = next_head;
            adc_data_ready = 1U;
        }
        else
        {
            adc_queue_overflow_count++;
        }
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
