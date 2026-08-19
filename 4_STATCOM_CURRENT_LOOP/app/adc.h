#ifndef APP_ADC_H_
#define APP_ADC_H_

#include "DSP28x_Project.h"

/*
 * The project runs SYSCLKOUT and EPWM1 TBCLK at 90 MHz. EPWM1 SOCA occurs
 * at 20 kHz when TBPRD is 4499 in up-count mode.
 * ADC采样中断频率
 */
#define ADC_SAMPLE_FREQ_HZ       20000UL
#define ADC_EPWM_TBCLK_HZ        90000000UL
#define ADC_EPWM_TBPRD           ((ADC_EPWM_TBCLK_HZ / ADC_SAMPLE_FREQ_HZ) - 1UL)

/*
 * Average 1024 consecutive samples (51.2 ms at the 20 kHz fast rate) once
 * after startup.  The power stage must be disabled and the AC-current sensor
 * must carry zero current.
 */
#define ADC_ZERO_CALIBRATION_SAMPLES 1024U

/*
 * ADC ISR只负责两路采样并按5:1固定抽取入队；前台批量排空队列。
 * 256组4 kHz缓存可覆盖约64 ms，可跨越SCI-B长回复与相邻SCI-A请求。
 */
#define ADC_PROCESS_QUEUE_SIZE       256U
#define ADC_PROCESS_QUEUE_MASK       (ADC_PROCESS_QUEUE_SIZE - 1U)
#define ADC_PROCESS_BATCH_LIMIT      128U

/* ADC保持20 kHz；ISR每5次固定入队1组，队列和PLL均严格运行在4 kHz。 */
#define ADC_QUEUE_DECIMATION         5U

#define ADC_CALIBRATION_IDLE         0U
#define ADC_CALIBRATION_RUNNING      1U
#define ADC_CALIBRATION_DONE         2U

typedef struct
{
    Uint16 vdc_raw;             /* SOC0: ADCINA0/VDC1, grid positive half-wave */
    Uint16 iac_raw;             /* SOC1: ADCINA2, schematic net IAC1 */
} ADC_SAMPLE;

typedef struct
{
    Uint16 iac_offset_count;    /* Integer zero-current ADC code */
    Uint16 sample_count;
    Uint16 state;
} ADC_ZERO_CALIBRATION;

extern volatile ADC_SAMPLE adc_sample;
extern volatile ADC_ZERO_CALIBRATION adc_zero_calibration;
extern volatile Uint16 adc_data_ready;
extern volatile Uint32 adc_sample_count;
extern volatile Uint32 adc_processed_count;
extern volatile Uint32 adc_pll_sample_count;
extern volatile Uint32 adc_overflow_count;
extern volatile Uint32 adc_queue_overflow_count;
extern volatile Uint32 adc_isr_last_cycles;
extern volatile Uint32 adc_isr_max_cycles;
extern volatile Uint32 adc_fast_current_count;
extern volatile int16 adc_fast_current_delta_count;
extern volatile Uint16 adc_fast_current_abs_count;
extern volatile Uint16 adc_fast_current_peak_count;
extern volatile Uint16 adc_fast_overcurrent_active;
extern volatile Uint16 adc_fast_overcurrent_latched;
extern volatile Uint16 adc_fast_overcurrent_confirm_count;
extern volatile Uint16 adc_fast_grid_raw;
extern volatile Uint16 adc_fast_grid_peak_raw;
extern volatile Uint16 adc_fast_grid_overvoltage_active;
extern volatile Uint16 adc_fast_grid_overvoltage_latched;
extern volatile Uint16 adc_fast_grid_overvoltage_confirm_count;
extern volatile float grid_halfwave_mean_counts;
extern volatile float grid_halfwave_centered_pu;
extern volatile Uint16 grid_halfwave_signal_valid;
extern volatile Uint16 grid_halfwave_clipped;

void adc_init(void);
void adc_start(void);
void adc_stop(void);
void adc_process(void);
/* Call with global interrupts disabled if recalibration is requested later. */
void adc_zero_calibration_start(void);
void adc_protection_clear_latched(void);
interrupt void AdcInt1Isr(void);

#endif /* APP_ADC_H_ */
