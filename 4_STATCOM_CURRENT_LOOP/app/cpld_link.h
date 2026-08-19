#ifndef APP_CPLD_LINK_H_
#define APP_CPLD_LINK_H_

#include "DSP28x_Project.h"

typedef struct
{
    Uint16 vdc_raw;
    Uint16 vdc_average;
    Uint16 temperature_count;
    Uint16 heartbeat;
    Uint16 state;
    Uint32 fault_bits;
    Uint16 command_echo;
    Uint16 remote_valid_frames;
    Uint16 remote_error_frames;
    Uint16 remote_uart_errors;
    Uint16 remote_crc_errors;
    Uint16 remote_incomplete_frames;
    Uint16 link_flags;
    Uint16 valid;
    Uint32 age_ticks;
} CPLD_LINK_STATUS;

typedef struct
{
    float modulation;
    Uint16 run_enable;
    Uint16 reset_request;
    Uint16 clear_fault_request;
    Uint16 vdc_over_limit_raw;
    Uint16 vdc_threshold_request;
} CPLD_LINK_COMMAND;

extern volatile CPLD_LINK_STATUS cpld_link_status;
extern volatile CPLD_LINK_COMMAND cpld_link_command;

void cpld_link_init(void);
void cpld_link_poll(void);
void cpld_link_force_stop(void);

#endif /* APP_CPLD_LINK_H_ */
