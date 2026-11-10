/* Exception vectors and the complete trap frame. The layout is TrapFrame in
 * include/vantage/arch/trap.h; arch/armv8/trap.c asserts these offsets. */
.equ FRAME_BYTES, 800
.equ FRAME_X30, 240
.equ FRAME_ELR, 256
.equ FRAME_FPCR, 272
.equ FRAME_V, 288

/* Every exception taken to EL1 uses SP_EL1: the interrupted task's own kernel
 * stack, or the boot/worker stack when no task is running. */
.macro trap_save
    sub sp, sp, #FRAME_BYTES
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    mrs x0, sp_el0
    stp x30, x0, [sp, #FRAME_X30]
    mrs x0, elr_el1
    mrs x1, spsr_el1
    stp x0, x1, [sp, #FRAME_ELR]
    mrs x0, fpcr
    mrs x1, fpsr
    stp x0, x1, [sp, #FRAME_FPCR]
    stp q0, q1, [sp, #FRAME_V + 0]
    stp q2, q3, [sp, #FRAME_V + 32]
    stp q4, q5, [sp, #FRAME_V + 64]
    stp q6, q7, [sp, #FRAME_V + 96]
    stp q8, q9, [sp, #FRAME_V + 128]
    stp q10, q11, [sp, #FRAME_V + 160]
    stp q12, q13, [sp, #FRAME_V + 192]
    stp q14, q15, [sp, #FRAME_V + 224]
    stp q16, q17, [sp, #FRAME_V + 256]
    stp q18, q19, [sp, #FRAME_V + 288]
    stp q20, q21, [sp, #FRAME_V + 320]
    stp q22, q23, [sp, #FRAME_V + 352]
    stp q24, q25, [sp, #FRAME_V + 384]
    stp q26, q27, [sp, #FRAME_V + 416]
    stp q28, q29, [sp, #FRAME_V + 448]
    stp q30, q31, [sp, #FRAME_V + 480]
.endm

.macro vector target
    .balign 0x80
    b \target
.endm

.section ".text"
.balign 0x800
.global vector_table
vector_table:
    /* Current EL with SP_EL0: never selected, the kernel runs on SP_EL1. */
    vector default_handler
    vector default_handler
    vector default_handler
    vector default_handler
    /* Current EL with SP_EL1. A synchronous kernel fault is not recoverable. */
    vector default_handler
    vector irq_exception
    vector default_handler
    vector default_handler
    /* Lower EL, AArch64. */
    vector el0_sync_exception
    vector irq_exception
    vector default_handler
    vector default_handler
    /* Lower EL, AArch32: unsupported. */
    vector default_handler
    vector default_handler
    vector default_handler
    vector default_handler

default_handler:
    wfe
    b default_handler

.global el0_sync_exception
.type el0_sync_exception, %function
el0_sync_exception:
    trap_save
    mov x0, sp
    bl trap_sync_el0
    b trap_restore
.size el0_sync_exception, . - el0_sync_exception

/* Taken from EL0 or from an interrupt-enabled EL1 region; never nested. */
.global irq_exception
.type irq_exception, %function
irq_exception:
    trap_save
    mov x0, sp
    bl trap_irq
    b trap_restore
.size irq_exception, . - irq_exception

/* void trap_return(const TrapFrame* frame): frame lies on the caller's stack. */
.global trap_return
.type trap_return, %function
trap_return:
    mov sp, x0
trap_restore:
    ldp q0, q1, [sp, #FRAME_V + 0]
    ldp q2, q3, [sp, #FRAME_V + 32]
    ldp q4, q5, [sp, #FRAME_V + 64]
    ldp q6, q7, [sp, #FRAME_V + 96]
    ldp q8, q9, [sp, #FRAME_V + 128]
    ldp q10, q11, [sp, #FRAME_V + 160]
    ldp q12, q13, [sp, #FRAME_V + 192]
    ldp q14, q15, [sp, #FRAME_V + 224]
    ldp q16, q17, [sp, #FRAME_V + 256]
    ldp q18, q19, [sp, #FRAME_V + 288]
    ldp q20, q21, [sp, #FRAME_V + 320]
    ldp q22, q23, [sp, #FRAME_V + 352]
    ldp q24, q25, [sp, #FRAME_V + 384]
    ldp q26, q27, [sp, #FRAME_V + 416]
    ldp q28, q29, [sp, #FRAME_V + 448]
    ldp q30, q31, [sp, #FRAME_V + 480]
    ldp x0, x1, [sp, #FRAME_FPCR]
    msr fpcr, x0
    msr fpsr, x1
    ldp x0, x1, [sp, #FRAME_ELR]
    msr elr_el1, x0
    msr spsr_el1, x1
    ldp x30, x0, [sp, #FRAME_X30]
    msr sp_el0, x0
    ldp x28, x29, [sp, #224]
    ldp x26, x27, [sp, #208]
    ldp x24, x25, [sp, #192]
    ldp x22, x23, [sp, #176]
    ldp x20, x21, [sp, #160]
    ldp x18, x19, [sp, #144]
    ldp x16, x17, [sp, #128]
    ldp x14, x15, [sp, #112]
    ldp x12, x13, [sp, #96]
    ldp x10, x11, [sp, #80]
    ldp x8, x9, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp, #0]
    add sp, sp, #FRAME_BYTES
    eret
.size trap_return, . - trap_return
