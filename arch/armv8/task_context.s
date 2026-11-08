.section .text
.balign 4
.global task_context_switch
.type task_context_switch, %function
task_context_switch:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x2, sp
    str x2, [x0, #96]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mrs x2, fpcr
    mrs x3, fpsr
    stp x2, x3, [x0, #168]
    // A task may yield from a syscall; don't lose its pending exception return.
    mrs x2, elr_el1
    mrs x3, spsr_el1
    stp x2, x3, [x0, #184]
    mrs x2, sp_el0
    str x2, [x0, #200]

    ldp x19, x20, [x1, #0]
    ldp x21, x22, [x1, #16]
    ldp x23, x24, [x1, #32]
    ldp x25, x26, [x1, #48]
    ldp x27, x28, [x1, #64]
    ldp x29, x30, [x1, #80]
    ldr x2, [x1, #96]
    mov sp, x2
    ldp d8, d9, [x1, #104]
    ldp d10, d11, [x1, #120]
    ldp d12, d13, [x1, #136]
    ldp d14, d15, [x1, #152]
    ldp x2, x3, [x1, #168]
    msr fpcr, x2
    msr fpsr, x3
    ldp x2, x3, [x1, #184]
    msr elr_el1, x2
    msr spsr_el1, x3
    ldr x2, [x1, #200]
    msr sp_el0, x2
    ret
.size task_context_switch, . - task_context_switch
