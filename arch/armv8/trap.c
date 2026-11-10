#include <vantage/arch/trap.h>
#include <vantage/arch/irq.h>
#include <vantage/kernel/syscall.h>
#include <vantage/kernel/task_port.h>

typedef char frame_size_must_match_assembly[(sizeof(TrapFrame) == 800) ? 1 : -1];
typedef char frame_x30_must_match[(__builtin_offsetof(TrapFrame, x[30]) == 240) ? 1 : -1];
typedef char frame_sp_must_match[(__builtin_offsetof(TrapFrame, sp_el0) == 248) ? 1 : -1];
typedef char frame_elr_must_match[(__builtin_offsetof(TrapFrame, elr) == 256) ? 1 : -1];
typedef char frame_fpcr_must_match[(__builtin_offsetof(TrapFrame, fpcr) == 272) ? 1 : -1];
typedef char frame_v_must_match[(__builtin_offsetof(TrapFrame, v) == 288) ? 1 : -1];

#define SYNDROME_CLASS_SHIFT 26
#define CLASS_SVC64 0x15U

void trap_sync_el0(TrapFrame* frame) {
    uint64_t syndrome;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(syndrome));
    if (((syndrome >> SYNDROME_CLASS_SHIFT) & 0x3fU) == CLASS_SVC64) {
        frame->x[0] = handle_sys_call(frame->x[8], frame->x[0]);
        return;
    }
    task_port_halt("[ERR] Unhandled EL0 exception; halted\n");
}

void trap_irq(TrapFrame* frame) {
    (void)frame;
    (void)irq_service();
}
