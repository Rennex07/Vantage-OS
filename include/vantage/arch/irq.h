#pragma once
#include <vantage/base.h>

/* Pi 4 GIC-400 (GICv2) and the per-core EL1 virtual timer, PPI 27. Requires
 * the firmware's default GIC handoff: interrupts in non-secure Group 1. */
#define IRQ_TIMER 27U
#define IRQ_NONE 1023U

/* Once per core, at EL1 with all DAIF masks set and the MMU enabled. Leaves
 * the timer stopped. Returns 0 when the timer interrupt cannot be enabled. */
int irq_init_core(void);
/* One-shot per-core deadline. The interrupt stays pending while the calling
 * core masks IRQs; stopping the timer withdraws it. */
void irq_timer_start(uint32_t microseconds);
void irq_timer_stop(void);
/* Acknowledges and completes one interrupt on the calling core. A timer
 * interrupt is stopped before completion. Returns its ID or IRQ_NONE. */
uint32_t irq_service(void);
