#include "protection.h"
#include "statcom_config.h"
#include "adc.h"
#include "grid_pll.h"
#include "cpld_link.h"

volatile STATCOM_PROTECTION_CONFIG statcom_protection_config =
{
    STATCOM_VDC_OV_DEFAULT_RAW,
    STATCOM_GRID_PEAK_OV_DEFAULT_RAW,
    STATCOM_IAC_OV_DEFAULT_DELTA_RAW
};

volatile Uint16 protection_grid_peak_trip_raw =
    STATCOM_GRID_PEAK_OV_DEFAULT_RAW;
volatile Uint16 protection_iac_trip_delta_count =
    STATCOM_IAC_OV_DEFAULT_DELTA_RAW;
volatile Uint16 dsp_protection_active_flags = 0U;
volatile Uint16 dsp_protection_latched_flags = 0U;

static Uint16 pll_was_locked = 0U;

void protection_init(void)
{
    statcom_protection_config.vdc_overvoltage_raw =
        STATCOM_VDC_OV_DEFAULT_RAW;
    statcom_protection_config.grid_peak_overvoltage_raw =
        STATCOM_GRID_PEAK_OV_DEFAULT_RAW;
    statcom_protection_config.iac_overcurrent_delta_raw =
        STATCOM_IAC_OV_DEFAULT_DELTA_RAW;
    protection_grid_peak_trip_raw = STATCOM_GRID_PEAK_OV_DEFAULT_RAW;
    protection_iac_trip_delta_count = STATCOM_IAC_OV_DEFAULT_DELTA_RAW;
    dsp_protection_active_flags = 0U;
    dsp_protection_latched_flags = 0U;
    pll_was_locked = 0U;
}

void protection_supervisor_step(void)
{
    Uint16 active = 0U;

    if(adc_fast_grid_overvoltage_active != 0U)
    {
        active |= DSP_FAULT_GRID_PEAK_OVERVOLTAGE;
    }
    if(adc_fast_overcurrent_active != 0U)
    {
        active |= DSP_FAULT_IAC_INSTANT_OVERCURRENT;
    }

    if(g_gridPllLocked != 0U)
    {
        pll_was_locked = 1U;
    }
    else if(pll_was_locked != 0U)
    {
        active |= DSP_FAULT_PLL_LOSS;
    }

    if(cpld_link_status.link_flags == 0x0002U)
    {
        active |= DSP_FAULT_CPLD_LINK;
    }
    if(cpld_link_status.fault_bits != 0UL)
    {
        active |= DSP_FAULT_CPLD_REPORTED;
    }

    dsp_protection_active_flags = active;
    dsp_protection_latched_flags |= active;
    if(adc_fast_grid_overvoltage_latched != 0U)
    {
        dsp_protection_latched_flags |= DSP_FAULT_GRID_PEAK_OVERVOLTAGE;
    }
    if(adc_fast_overcurrent_latched != 0U)
    {
        dsp_protection_latched_flags |= DSP_FAULT_IAC_INSTANT_OVERCURRENT;
    }
}

Uint16 protection_read_threshold(Uint16 address, Uint16 *value)
{
    switch(address)
    {
    case PROTECTION_REG_VDC_OV_RAW:
        *value = statcom_protection_config.vdc_overvoltage_raw;
        break;
    case PROTECTION_REG_GRID_PEAK_OV_RAW:
        *value = statcom_protection_config.grid_peak_overvoltage_raw;
        break;
    case PROTECTION_REG_IAC_OV_DELTA_RAW:
        *value = statcom_protection_config.iac_overcurrent_delta_raw;
        break;
    default:
        return 0U;
    }
    return 1U;
}

Uint16 protection_write_threshold(Uint16 address, Uint16 value)
{
    if(value == 0U)
    {
        return 2U;
    }

    switch(address)
    {
    case PROTECTION_REG_VDC_OV_RAW:
        if(value > STATCOM_VDC_OV_DEFAULT_RAW) return 2U;
        statcom_protection_config.vdc_overvoltage_raw = value;
        break;
    case PROTECTION_REG_GRID_PEAK_OV_RAW:
        if(value > STATCOM_GRID_PEAK_OV_DEFAULT_RAW) return 2U;
        statcom_protection_config.grid_peak_overvoltage_raw = value;
        protection_grid_peak_trip_raw = value;
        break;
    case PROTECTION_REG_IAC_OV_DELTA_RAW:
        if(value > STATCOM_IAC_OV_DEFAULT_DELTA_RAW) return 2U;
        statcom_protection_config.iac_overcurrent_delta_raw = value;
        protection_iac_trip_delta_count = value;
        break;
    default:
        return 0U;
    }
    return 1U;
}

void protection_clear_latched(void)
{
    adc_protection_clear_latched();
    /* A STOP-state clear command rearms PLL-loss supervision.  If the PLL is
     * already locked, protection_supervisor_step() immediately arms it again;
     * if it is unlocked, the system may reacquire without an uncleareable
     * historical loss fault.  START remains blocked until lock is restored. */
    pll_was_locked = 0U;
    dsp_protection_latched_flags = 0U;
    protection_supervisor_step();
}
