/* Pi 4 firmware spin-table slots: core 1 = 0xe0, 2 = 0xe8, 3 = 0xf0. */
.equ SPIN_TABLE_BASE, 0xd8
.equ SYSTEM_TIMER_LOW, 0xfe003004
.equ SECONDARY_TIMEOUT_US, 1000000
.equ SECONDARY_WAIT_LIMIT, 0x4000000

.section ".text"
.global secondary_core_start
.type secondary_core_start, %function
secondary_core_start:
    /* Core 0 calls once per secondary at EL1 before its MMU/data cache is on.
     * w0 = core ID (1..3); w0 returns 1 if that core reaches the entry signal. */
    cmp w0, #1
    b.lo .Lfailed
    cmp w0, #3
    b.hi .Lfailed
    mrs x1, CurrentEL
    cmp x1, #4
    b.ne .Lfailed
    mrs x1, mpidr_el1
    ubfx x1, x1, #0, #8
    cbnz x1, .Lfailed
    mrs x1, sctlr_el1
    tbnz x1, #0, .Lfailed
    tbnz x1, #2, .Lfailed
    mov w7, w0

    ldr x1, =secondary_ready
    add x1, x1, x7, lsl #6
    str wzr, [x1]
    dsb sy

    ldr x2, =secondary_entry
    mov x3, #SPIN_TABLE_BASE
    add x3, x3, x7, lsl #3
    str x2, [x3]
    dsb sy
    sev

    ldr x3, =SYSTEM_TIMER_LOW
    ldr w4, [x3]
    ldr w5, =SECONDARY_TIMEOUT_US
    ldr w2, =SECONDARY_WAIT_LIMIT
.Lwait:
    ldr w0, [x1]
    cbnz w0, .Lready
    ldr w6, [x3]
    sub w6, w6, w4
    cmp w6, w5
    b.hs .Lfailed
    subs w2, w2, #1
    b.ne .Lwait
.Lfailed:
    mov w0, #0
    ret
.Lready:
    dsb sy
    mov w0, #1
    ret
.size secondary_core_start, . - secondary_core_start

.type secondary_entry, %function
secondary_entry:
    /* Firmware must enter with translation and data cache disabled. */
    mrs x0, CurrentEL
    cmp x0, #8
    b.eq .Lcheck_el2
    cmp x0, #4
    b.ne .Lpark
    mrs x0, sctlr_el1
    b .Lcheck_control
.Lcheck_el2:
    mrs x0, sctlr_el2
.Lcheck_control:
    tbnz x0, #0, .Lpark
    tbnz x0, #2, .Lpark
    mrs x0, mpidr_el1
    ubfx x1, x0, #0, #8
    cmp x1, #1
    b.lo .Lpark
    cmp x1, #3
    b.hi .Lpark
    msr daifset, #0xf
    mrs x0, CurrentEL
    cmp x0, #8
    b.ne .Lsecondary_el1
    /* Match the primary's EL2 -> EL1h transition before calling C. */
    mov x0, #0x33ff
    msr cptr_el2, x0
    mov x0, #(1 << 31)
    msr hcr_el2, x0
    mov x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    ldr x0, =0x30D00800
    msr sctlr_el1, x0
    mov x0, #0x3c5
    msr spsr_el2, x0
    ldr x0, =.Lsecondary_el1
    msr elr_el2, x0
    eret
.Lsecondary_el1:
    msr spsel, #1
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    isb
    ldr x0, =vector_table
    msr vbar_el1, x0
    isb
    ldr x0, =secondary_stack_tops
    ldr x0, [x0, x1, lsl #3]
    mov sp, x0
    /* Exercise the reserved stack before reporting that the entry was reached. */
    stp xzr, xzr, [sp, #-16]!
    ldp x2, x3, [sp], #16
    ldr x0, =secondary_ready
    add x0, x0, x1, lsl #6
    mov w2, #1
    str w2, [x0]
    dsb sy
    sev
    mov x19, x1
    /* No cached job/state access until the primary publishes immutable tables.
     * This gate stays Device-mapped after the primary enables its MMU. */
    ldr x0, =secondary_mmu_ready
.Lwait_mmu:
    ldr w2, [x0]
    cbnz w2, .Ljoin_workers
    wfe
    b .Lwait_mmu
.Ljoin_workers:
    dsb sy
    bl mmu_enable_shared
    /* Without its timer interrupt this core could not preempt user code. */
    bl irq_init_core
    cbz w0, .Lpark
    mov w0, w19
    bl parallel_worker_entry
.Lpark:
    wfe
    b .Lpark
.size secondary_entry, . - secondary_entry

.section ".rodata"
.balign 8
secondary_stack_tops:
    .quad 0 /* Core 0 never enters here. */
    .quad __core1_stack_top
    .quad __core2_stack_top
    .quad __core3_stack_top

.section ".bss"
.balign 64
secondary_ready:
    /* One cache-line-sized slot per core; only startup uses these flags. */
    .skip 4 * 64

.section ".smp.boot", "aw", %nobits
.balign 64
.global secondary_mmu_ready
secondary_mmu_ready:
    .skip 64
