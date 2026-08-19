#ifndef APP_STATCOM_CONFIG_H_
#define APP_STATCOM_CONFIG_H_

/* Confirmed hardware and simulation baseline. */
#define STATCOM_GRID_NOMINAL_RMS_V       (220.0f)
#define STATCOM_GRID_NOMINAL_FREQ_HZ     (50.0f)
#define STATCOM_REACTOR_H                (0.0005f)
#define STATCOM_DC_CAPACITANCE_F         (0.0026f) /* 4 x 650 uF parallel */
#define STATCOM_CONTROL_RATE_HZ          (20000.0f)
#define STATCOM_MODULATION_LIMIT         (0.95f)

/* Low-voltage commissioning starting point; not a rated operating point. */
#define STATCOM_COMMISSION_GRID_RMS_V    (50.0f)
#define STATCOM_COMMISSION_VDC_REF_V     (80.0f)
#define STATCOM_COMMISSION_IPEAK_A       (3.0f)

/* PLECS starting values. They remain disabled until shadow-mode validation. */
#define STATCOM_PI_KP_START              (0.15f)
#define STATCOM_PI_KI_START              (0.8f)
#define STATCOM_PR_KP_START              (0.8f)
#define STATCOM_PR_KR_START              (10.0f)
#define STATCOM_PR_WC_RAD_S              (31.4159265f)  /* 2*pi*5 */
#define STATCOM_PR_W0_RAD_S              (314.159265f)  /* 2*pi*50 */

/* Safe build policy: no PWM/run command may leave the DSP in this stage. */
#define STATCOM_POWER_OUTPUT_ENABLED     (0U)
#define STATCOM_ACTIVE_STAGE             (2U)

/*
 * Stage-2 fast-current diagnostics.  The 5 A value is a provisional
 * commissioning threshold above the 3 A shadow-mode target.  It must be
 * replaced by a value derived from physical current-channel calibration
 * before any power-output review.  This monitor only latches diagnostics;
 * it cannot release or drive PWM while STATCOM_POWER_OUTPUT_ENABLED is zero.
 */
#define STATCOM_FAST_OVERCURRENT_MONITOR_ENABLED  (1U)
#define STATCOM_FAST_OVERCURRENT_CONFIRM_SAMPLES  (3U)
#define STATCOM_FAST_GRID_OV_CONFIRM_SAMPLES      (3U)

/*
 * A/B diagnosis only.  Keep zero in the standard image.  Setting one moves
 * current conversion/calibration back to the 4 kHz foreground and removes all
 * fast-protection work from the 20 kHz ADC ISR.  Power output remains locked.
 */
#define STATCOM_FAST_PROTECTION_DIAGNOSTIC_BYPASS (0U)

/* V0x0105 raw-code thresholds. Users may lower but not exceed these limits. */
#define STATCOM_VDC_OV_DEFAULT_RAW                 (3410U)
#define STATCOM_GRID_PEAK_OV_DEFAULT_RAW           (1313U)
#define STATCOM_IAC_OV_DEFAULT_DELTA_RAW            (51U)

/*
 * ADCINA0仍由EPWM以20 kHz采样；ISR每5点确定性抽取一次，SOGI-PLL以4 kHz运行。
 * 窗口和状态计数均按4 kHz换算，保持20 ms均值窗口、37.5 ms丢失判断、
 * 100 ms锁定确认和20 ms解锁确认。
 */
#define GRID_ADC_REFERENCE_VOLTS             (3.0F)
#define GRID_ADC_FULL_SCALE_COUNTS           (4096.0F)
#define GRID_INPUT_FULL_SCALE_VOLTS          (1123.0F)
#define GRID_NOMINAL_PEAK_VOLTS              (311.12698F)
#define GRID_SAMPLE_FREQUENCY_HZ             (4000.0F)
#define GRID_SAMPLE_PERIOD_SECONDS           (0.00025F)
#define GRID_DC_WINDOW_SAMPLES               (80U)
#define GRID_SIGNAL_LOSS_SAMPLES             (150U)
#define GRID_SIGNAL_PRESENT_COUNTS           (32U)
#define GRID_ADC_CLIP_HIGH_COUNTS            (4090U)
#define GRID_ADC_CLIP_CONFIRM_SAMPLES        (4U)
#define GRID_PLL_NOMINAL_FREQUENCY_HZ        (50.0F)
#define GRID_PLL_MIN_FREQUENCY_HZ            (45.0F)
#define GRID_PLL_MAX_FREQUENCY_HZ            (55.0F)
#define GRID_PLL_TRACK_MIN_FREQUENCY_HZ      (44.0F)
#define GRID_PLL_TRACK_MAX_FREQUENCY_HZ      (56.0F)
#define GRID_PLL_LOOP_BANDWIDTH_HZ           (20.0F)
#define GRID_PLL_DAMPING                     (0.70710678F)
#define GRID_PLL_SOGI_K                      (1.41421356F)
#define GRID_PLL_LOCK_Q_ERROR_PU             (0.06F)
#define GRID_PLL_UNLOCK_Q_ERROR_PU           (0.20F)
#define GRID_PLL_SIGNAL_VALID_AMPLITUDE_PU   (0.05F)
#define GRID_PLL_NORMALIZATION_FLOOR_PU      (0.08F)
#define GRID_PLL_LOCK_SAMPLES                (400U)
#define GRID_PLL_UNLOCK_SAMPLES              (80U)

#endif /* APP_STATCOM_CONFIG_H_ */
