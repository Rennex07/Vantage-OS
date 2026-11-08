/* BCM2711 system timer: coherent high/low/high sampling of its 64-bit counter. */
#include "delay.h"
#include "../../../includes/io.h"

#define TIMER_LOW (MMIO_BASE + 0x3004UL)
#define TIMER_HIGH (MMIO_BASE + 0x3008UL)

uint64_t get_sys_timer(void) {
    uint32_t high, low, check;
    do {
        high = mmio_read(TIMER_HIGH);
        low = mmio_read(TIMER_LOW);
        check = mmio_read(TIMER_HIGH);
    } while (high != check);
    return ((uint64_t)high << 32) | low;
}

static void wait_microseconds(uint64_t duration) {
    uint64_t start;
    if (!duration) return;
    start = get_sys_timer();
    while (get_sys_timer() - start < duration) {
#ifndef VANTAGE_SIM
        __asm__ volatile("yield" ::: "memory");
#endif
    }
}

void wait_usec(uint32_t microseconds) { wait_microseconds(microseconds); }
void wait_msec(unsigned int milliseconds) { wait_microseconds((uint64_t)milliseconds * 1000); }
void wait_msec_st(unsigned int milliseconds) { wait_msec(milliseconds); }

void wait_cycles(unsigned int iterations) {
    while (iterations--) {
#ifndef VANTAGE_SIM
        __asm__ volatile("nop");
#endif
    }
}
