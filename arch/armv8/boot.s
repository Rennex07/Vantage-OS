/* Source:
        https://rpi4os.com/part1-bootstrapping/
*/

.section ".text.boot"
.global _start

_start:
    // Zero checker for processor ID, else hang
    mrs x1, mpidr_el1
    and x1, x1, #3
    cbz x1, 2f
    
1: // Hang would make an inf loop 
    wfe
    b 1b

2:
    // Preserve the firmware DTB pointer across EL setup and BSS clearing.
    mov x20, x0
    msr daifset, #0xf
    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    bne el1_entry

    mov x0, #0x33ff
    msr cptr_el2, x0
    mov x0, #(1 << 31)
    msr hcr_el2, x0
    // EL1 owns the generic timers; the virtual count equals the physical one.
    mov x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    ldr x0, =0x30D00800
    msr sctlr_el1, x0
    mov x0, #0x3c5
    msr spsr_el2, x0
    ldr x0, =el1_entry
    msr elr_el2, x0
    eret

el1_entry:
    msr spsel, #1
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    // Check the complete DTB before writing even the file-backed boot stack.
    mrs x0, sctlr_el1
    tbnz x0, #0, boot_memory_rejected
    tbnz x0, #2, boot_memory_rejected
    ldr x1, =_end
    cmp x20, x1
    b.lo boot_memory_rejected
    tst x20, #7
    b.ne boot_memory_rejected
    mov x2, #0xfc000000
    sub x3, x2, #40
    cmp x20, x3
    b.hi boot_memory_rejected
    ldr w3, [x20]
    rev w3, w3
    ldr w4, =0xd00dfeed
    cmp w3, w4
    b.ne boot_memory_rejected
    ldr w3, [x20, #4]
    rev w3, w3
    cmp w3, #40
    b.lo boot_memory_rejected
    mov w4, #0x200000
    cmp w3, w4
    b.hi boot_memory_rejected
    add x3, x20, x3
    cmp x3, x2
    b.hi boot_memory_rejected

    ldr x0, =vector_table
    msr vbar_el1, x0
    isb
    // This stack is in the loaded image, not the unchecked zero-fill tail.
    ldr x1, =__boot_stack_top
    mov sp, x1
    mov x0, x20
    ldr x1, =__bss_start
    ldr x2, =__bss_end
    bl mm_boot_validate
    cbz w0, boot_memory_rejected

    ldr x1, =__kernel_stack_top
    mov sp, x1

    ldr x1, =__bss_start
    ldr w2, =__bss_size
3:
    cbz w2, 4f
    str xzr, [x1], #8
    sub w2, w2, #1
    cbnz w2, 3b

4:
    mov x0, x20
    bl main
    b 1b

.global boot_memory_rejected
boot_memory_rejected:
    // UART initialization needs validated memory. Reject without further writes.
    wfe
    b boot_memory_rejected
