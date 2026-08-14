#ifndef APP_ADC_H_
#define APP_ADC_H_

#include "DSP28x_Project.h"

/*
 * The project runs SYSCLKOUT and EPWM1 TBCLK at 90 MHz. EPWM1 SOCA occurs
 * at 10 kHz when TBPRD is 8999 in up-count mode.
 * ADC采样中断频率
 */
#define ADC_SAMPLE_FREQ_HZ       10000UL
#define ADC_EPWM_TBCLK_HZ        90000000UL
#define ADC_EPWM_TBPRD           ((ADC_EPWM_TBCLK_HZ / ADC_SAMPLE_FREQ_HZ) - 1UL)

/* External REF2030 reference connected between VREFHI and VREFLO. */
#define ADC_REFERENCE_VOLTAGE        3.0f
#define ADC_FULL_SCALE_COUNTS        4096.0f
#define ADC_VOLTS_PER_COUNT          (ADC_REFERENCE_VOLTAGE / ADC_FULL_SCALE_COUNTS)

/* Conversion ratios calculated from the analog front end schematic. */
#define ADC_VDC_VOLTS_PER_COUNT      0.2741878f
#define ADC_CURRENT_AMPS_PER_COUNT   0.09887695f

/*
 * First-order low-pass for the AC-current waveform.  A 500 Hz cutoff keeps
 * 50/60 Hz amplitude error below 1% while suppressing switching/ADC noise.
 * alpha = 1 - exp(-2*pi*fc/fs), for fs = 10 kHz and fc = 500 Hz.
 */
#define ADC_IAC_FILTER_CUTOFF_HZ      500.0f
#define ADC_IAC_FILTER_ALPHA          0.2695973f

/* 200 ms contains exactly 10 cycles at 50 Hz and 12 cycles at 60 Hz. */
#define ADC_IAC_RMS_WINDOW_SAMPLES    2000U

/*
 * Average 1024 samples (102.4 ms at 10 kHz) once after startup.  The power
 * stage must be disabled and both current sensors must carry zero current.
 */
#define ADC_ZERO_CALIBRATION_SAMPLES 1024U

#define ADC_CALIBRATION_IDLE         0U
#define ADC_CALIBRATION_RUNNING      1U
#define ADC_CALIBRATION_DONE         2U

typedef struct
{
    Uint16 vdc_raw;             /* SOC0: ADCINA0, schematic net VDC1 */
    Uint16 idc_raw;             /* SOC1: ADCINA1, schematic net IDC1 */
    Uint16 iac_raw;             /* SOC2: ADCINA2, schematic net IAC1 */
} ADC_SAMPLE;

typedef struct
{
    float vdc_pin_v;            /* Voltage present on ADCINA0 */
    float idc_pin_v;            /* Voltage present on ADCINA1 */
    float iac_pin_v;            /* Voltage present on ADCINA2 */
    float vdc_bus_v;            /* Real DC bus voltage */
    float idc_a;                /* Real DC current */
    float iac_a;                /* Real AC current */
    float iac_filtered_a;       /* Low-pass-filtered AC-current waveform */
    float iac_rms_a;            /* 200 ms true RMS, residual DC removed */
} ADC_ANALOG_VALUE;

typedef struct
{
    Uint16 rms_sample_count;    /* Samples accumulated in current RMS window */
    Uint16 rms_valid;           /* 1 after the first complete 200 ms window */
    Uint32 rms_update_count;    /* Number of completed RMS windows */
    Uint32 processed_count;     /* Samples consumed by adc_process() */
    Uint32 dropped_count;       /* ISR samples overwritten before processing */
} ADC_IAC_MEASUREMENT_STATUS;

typedef struct
{
    float idc_offset_count;
    float iac_offset_count;
    Uint16 sample_count;
    Uint16 state;
} ADC_ZERO_CALIBRATION;

extern volatile ADC_SAMPLE adc_sample;
extern volatile ADC_ANALOG_VALUE adc_value;
extern volatile ADC_ZERO_CALIBRATION adc_zero_calibration;
extern volatile ADC_IAC_MEASUREMENT_STATUS adc_iac_status;
extern volatile Uint16 adc_data_ready;
extern volatile Uint32 adc_sample_count;
extern volatile Uint32 adc_overflow_count;

void adc_init(void);
void adc_start(void);
void adc_stop(void);
void adc_process(void);
/* Call with global interrupts disabled if recalibration is requested later. */
void adc_zero_calibration_start(void);
interrupt void AdcInt1Isr(void);

#endif /* APP_ADC_H_ */
