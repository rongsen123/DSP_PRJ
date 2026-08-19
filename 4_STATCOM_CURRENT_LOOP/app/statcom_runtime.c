#include "statcom_runtime.h"
#include "statcom_config.h"
#include "adc.h"
#include "cpld_link.h"
#include "grid_pll.h"
#include "protection.h"

volatile STATCOM_RUNTIME statcom_runtime =
{
    STATCOM_STATE_SAFE_BOOT,
    0U, 0U, 0U, 0U,
    0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    0U, 0U, 0UL
};

void statcom_runtime_init(void)
{
    protection_init();
    cpld_link_init();
    cpld_link_command.vdc_over_limit_raw =
        statcom_protection_config.vdc_overvoltage_raw;
    cpld_link_command.vdc_threshold_request = 1U;
    statcom_runtime.state = STATCOM_STATE_ADC_VERIFY;
    statcom_runtime.output_permitted = 0U;
    statcom_runtime.modulation = 0.0f;
}

void statcom_runtime_step(void)
{
    protection_supervisor_step();
    /* ADC-derived measurements remain raw; engineering conversion is host-side. */
    statcom_runtime.grid_halfwave_raw = adc_sample.vdc_raw;
    statcom_runtime.grid_current_raw = adc_sample.iac_raw;
    statcom_runtime.grid_current_zero_raw =
        adc_zero_calibration.iac_offset_count;
    statcom_runtime.dc_link_raw = cpld_link_status.vdc_raw;
    statcom_runtime.theta_rad = g_gridPllThetaRad;
    statcom_runtime.frequency_hz = g_gridPllFrequencyHz;
    statcom_runtime.pll_locked = g_gridPllLocked;
    statcom_runtime.loop_count++;

    if(dsp_protection_latched_flags != 0U)
    {
        statcom_runtime.state = STATCOM_STATE_FAULT;
    }
    else if(statcom_runtime.state == STATCOM_STATE_FAULT)
    {
        statcom_runtime.state = STATCOM_STATE_ADC_VERIFY;
    }

#if STATCOM_POWER_OUTPUT_ENABLED == 0U
    statcom_runtime.output_permitted = 0U;
    statcom_runtime.modulation = 0.0f;
    /* Debug START may be transported and echoed, but never becomes output. */
#endif
}
