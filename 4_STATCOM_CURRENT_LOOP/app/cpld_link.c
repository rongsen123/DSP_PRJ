#include "cpld_link.h"

volatile CPLD_LINK_STATUS cpld_link_status =
{
    0, 0, 0, 0, 0, 0, 0, 0,
    0,
    0, 0, 0, 0, 0,
    0
};

volatile CPLD_LINK_COMMAND cpld_link_command =
{
    0.0, 0, 0, 0, 3410U, 1U
};

void cpld_link_force_stop(void)
{
    cpld_link_command.modulation = 0.0;
    cpld_link_command.run_enable = 0;
    cpld_link_command.reset_request = 0;
    cpld_link_command.clear_fault_request = 0;
}

void cpld_link_init(void)
{
    cpld_link_status.valid = 0U;
    cpld_link_status.age_ticks = 0UL;
    cpld_link_force_stop();
    cpld_link_command.vdc_over_limit_raw = 3410U;
    cpld_link_command.vdc_threshold_request = 1U;
}

void cpld_link_poll(void)
{
    /* The Modbus task in sci.c owns link age, status and command forwarding. */
}
