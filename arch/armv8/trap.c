#include <vantage/arch/trap.h>
#include <vantage/arch/irq.h>
#include <vantage/kernel/process.h>
#include <vantage/kernel/syscall.h>
#include <vantage/kernel/task_port.h>

typedef char frame_size_must_match_assembly[(sizeof(TrapFrame) == 800) ? 1 : -1];
typedef char frame_x30_must_match[(__builtin_offsetof(TrapFrame, x[30]) == 240) ? 1 : -1];
typedef char frame_sp_must_match[(__builtin_offsetof(TrapFrame, sp_el0) == 248) ? 1 : -1];
typedef char frame_elr_must_match[(__builtin_offsetof(TrapFrame, elr) == 256) ? 1 : -1];
typedef char frame_fpcr_must_match[(__builtin_offsetof(TrapFrame, fpcr) == 272) ? 1 : -1];
typedef char frame_v_must_match[(__builtin_offsetof(TrapFrame, v) == 288) ? 1 : -1];

#define SYNDROME_CLASS_SHIFT 26
#define CLASS_UNKNOWN 0x00U
#define CLASS_SYSTEM_REGISTER 0x18U
#define CLASS_SVC64 0x15U
#define CLASS_INSTRUCTION_ABORT_EL0 0x20U
#define CLASS_DATA_ABORT_EL0 0x24U

void trap_sync_el0(TrapFrame* frame) {
    uint64_t syndrome;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(syndrome));
    unsigned int class = (unsigned int)((syndrome >> SYNDROME_CLASS_SHIFT) & 0x3fU);
    task_trap_boundary();
    if (class == CLASS_SVC64) {
        frame->x[0] = handle_sys_call(frame->x[8], frame->x[0]);
        task_trap_boundary();
        return;
    }
    /* Any other synchronous exception ends the offending process only. */
    process_fault(class == CLASS_DATA_ABORT_EL0 ? PROCESS_FAULT_DATA :
                  class == CLASS_INSTRUCTION_ABORT_EL0 ? PROCESS_FAULT_INSTRUCTION :
                  class == CLASS_UNKNOWN || class == CLASS_SYSTEM_REGISTER ? PROCESS_FAULT_UNDEFINED :
                  PROCESS_FAULT_OTHER);
    task_port_halt("[ERR] EL0 exception outside a process; halted\n");
}

void trap_irq(TrapFrame* frame) {
    (void)frame;
    /* The interrupt is completed on this core before the task can migrate. */
    if (irq_service() == IRQ_TIMER) task_tick();
    task_trap_boundary();
}
