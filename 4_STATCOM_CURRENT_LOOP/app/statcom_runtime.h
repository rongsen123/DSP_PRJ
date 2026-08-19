#ifndef APP_STATCOM_RUNTIME_H_
#define APP_STATCOM_RUNTIME_H_

#include "DSP28x_Project.h"

typedef enum
{
    STATCOM_STATE_SAFE_BOOT = 0,
    STATCOM_STATE_ADC_VERIFY,
    STATCOM_STATE_LINK_VERIFY,
    STATCOM_STATE_PLL_MONITOR,
    STATCOM_STATE_CONTROL_SHADOW,
    STATCOM_STATE_CURRENT_LOOP,
    STATCOM_STATE_VOLTAGE_LOOP,
    STATCOM_STATE_RUN,
    STATCOM_STATE_FAULT
} STATCOM_STATE;

typedef struct
{
    STATCOM_STATE state;
    Uint16 grid_halfwave_raw;
    Uint16 grid_current_raw;
    Uint16 grid_current_zero_raw;
    Uint16 dc_link_raw;
    float theta_rad;
    float frequency_hz;
    float ip_ref_a;
    float iq_ref_a;
    float current_ref_a;
    float modulation;
    Uint16 pll_locked;
    Uint16 output_permitted;
    Uint32 loop_count;
} STATCOM_RUNTIME;

extern volatile STATCOM_RUNTIME statcom_runtime;

void statcom_runtime_init(void);
void statcom_runtime_step(void);

#endif /* APP_STATCOM_RUNTIME_H_ */
