#pragma once
#include "task.h"

/* AAPCS64 cooperative call boundary, not an interrupt/exception frame.
 * Keep offsets in arch/armv8/task_context.s in sync. */
typedef struct {
    uint64_t x19_x30[12];
    uint64_t sp;
    uint64_t d8_d15[8];
    uint64_t fpcr, fpsr;
    uint64_t elr, spsr, sp_el0;
} TaskContext;

int task_port_owned(void);
unsigned int task_port_core(void);
void task_port_lock(uint32_t* lock);
void task_port_unlock(uint32_t* lock);
void task_context_init(TaskContext* context, void* stack, size_t size,
                       void (*entry)(void));
void task_context_dispose(TaskContext* context);
void task_context_switch(TaskContext* previous, const TaskContext* next);
void task_port_halt(const char* message);
