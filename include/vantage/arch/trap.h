#pragma once
#include <vantage/base.h>

/* Complete register state of an interrupted EL0 or EL1 context, stored on the
 * interrupted task's kernel stack. Keep arch/armv8/exception.s in sync. */
typedef struct {
    uint64_t x[31];
    uint64_t sp_el0;
    uint64_t elr, spsr;
    uint64_t fpcr, fpsr;
    uint64_t v[32][2];
} TrapFrame;

/* Called by the exception vectors with all DAIF masks set. */
void trap_sync_el0(TrapFrame* frame);
void trap_irq(TrapFrame* frame);
/* Loads every register from a frame on the caller's kernel stack and performs
 * the exception return. Does not return. */
void trap_return(const TrapFrame* frame);
