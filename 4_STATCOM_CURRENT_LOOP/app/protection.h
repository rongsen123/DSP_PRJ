#ifndef APP_PROTECTION_H_
#define APP_PROTECTION_H_

#include "DSP28x_Project.h"

/* DSP fault bits are transported in the high 16 bits of FC04 000C/000D. */
#define DSP_FAULT_GRID_PEAK_OVERVOLTAGE  (0x0001U)
#define DSP_FAULT_IAC_INSTANT_OVERCURRENT (0x0002U)
#define DSP_FAULT_PLL_LOSS               (0x0004U)
#define DSP_FAULT_CPLD_LINK               (0x0008U)
#define DSP_FAULT_CPLD_REPORTED           (0x0010U)

/* User-visible holding registers.  Limits remain fixed in firmware. */
#define PROTECTION_REG_VDC_OV_RAW         (0x1000U) /* CPLD ADC absolute code */
#define PROTECTION_REG_GRID_PEAK_OV_RAW   (0x1001U) /* ADCINA0 absolute code */
#define PROTECTION_REG_IAC_OV_DELTA_RAW   (0x1002U) /* |ADCINA2-zero| counts */

typedef struct
{
    Uint16 vdc_overvoltage_raw;
    Uint16 grid_peak_overvoltage_raw;
    Uint16 iac_overcurrent_delta_raw;
} STATCOM_PROTECTION_CONFIG;

extern volatile STATCOM_PROTECTION_CONFIG statcom_protection_config;
extern volatile Uint16 protection_grid_peak_trip_raw;
extern volatile Uint16 protection_iac_trip_delta_count;
extern volatile Uint16 dsp_protection_active_flags;
extern volatile Uint16 dsp_protection_latched_flags;

void protection_init(void);
void protection_supervisor_step(void);
Uint16 protection_read_threshold(Uint16 address, Uint16 *value);
Uint16 protection_write_threshold(Uint16 address, Uint16 value);
void protection_clear_latched(void);

#endif /* APP_PROTECTION_H_ */
