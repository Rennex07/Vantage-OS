#pragma once
#include <vantage/kernel/task.h>

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
/* One-shot time slice on the calling core. Its interrupt stays pending while
 * IRQs are masked and is withdrawn by stop. Called from the dispatcher. */
void task_port_slice_start(uint32_t microseconds);
void task_port_slice_stop(void);
/* IRQ mask of the calling context; every other exception stays masked. */
void task_port_irq_enable(void);
void task_port_irq_disable(void);
/* Upcalls from the trap layer, at EL1 with all exceptions masked, on the
 * interrupted task's kernel stack. task_tick returns once the task is
 * dispatched again, possibly on another core. task_trap_boundary is called
 * on every kernel entry from and return to EL0 and does not return for a
 * thread whose process has been terminated. */
void task_tick(void);
void task_trap_boundary(void);
/* Installs a process root (with its ASID) as the calling core's user half;
 * 0 leaves only the kernel mapped. Called with the scheduler lock held. */
void task_port_address_space(uint64_t root);
/* Enters EL0 on the calling task's kernel stack with every other register
 * cleared. Does not return on ARM. */
void task_port_enter_user(uint64_t entry, uint64_t stack, uint64_t argument);
