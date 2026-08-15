#include "statcom_runtime.h"
#include "statcom_config.h"
#include "adc.h"
#include "cpld_link.h"
#include "grid_pll.h"

volatile STATCOM_RUNTIME statcom_runtime =
{
    STATCOM_STATE_SAFE_BOOT,
    0.0f, 0.0f, 0.0f,
    0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    0U, 0U, 0UL
};

void statcom_runtime_init(void)
{
    cpld_link_init();
    statcom_runtime.state = STATCOM_STATE_ADC_VERIFY;
    statcom_runtime.output_permitted = 0U;
    statcom_runtime.modulation = 0.0f;
}

void statcom_runtime_step(void)
{
    /* Stage 1 publishes measurements and real PLL diagnostics only. */
    statcom_runtime.grid_halfwave_v = adc_value.vdc_bus_v;
    statcom_runtime.grid_current_a = adc_value.iac_a;
    statcom_runtime.dc_link_v = cpld_link_status.vdc_v;
    statcom_runtime.theta_rad = g_gridPllThetaRad;
    statcom_runtime.frequency_hz = g_gridPllFrequencyHz;
    statcom_runtime.pll_locked = g_gridPllLocked;
    statcom_runtime.loop_count++;

#if STATCOM_POWER_OUTPUT_ENABLED == 0U
    statcom_runtime.output_permitted = 0U;
    statcom_runtime.modulation = 0.0f;
    /* Debug START may be transported and echoed, but never becomes output. */
#endif
}
