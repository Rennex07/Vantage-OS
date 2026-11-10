#pragma once
#include <vantage/base.h>

/* BCM2711's 1 MHz system timer; time is in microseconds, including rollover. */
uint64_t get_sys_timer(void);
void wait_usec(uint32_t microseconds);
void wait_msec(unsigned int milliseconds);
void wait_msec_st(unsigned int milliseconds);
/* Instruction iterations, not a calibrated duration. */
void wait_cycles(unsigned int iterations);
