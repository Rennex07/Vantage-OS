SECTIONS {
    . = 0x80000; /* Kernel load address */
    __kernel_start = .;
    .text : {
        KEEP(*(.text.boot))
        *(.text .text* .gnu.linkonce.t*)
    }
    .rodata : {
        *(.rodata .rodata.* .gnu.linkonce.r*)
    }
    ASSERT(. <= 0x200000, "Shared executable region exceeds its 2 MiB mapping")
    . = ALIGN(0x200000);
    .data : {
        *(.data .data.*)
    }
    /* File-backed scratch space is already part of the firmware-loaded image.
     * It must not come from the as-yet-unvalidated NOLOAD zero-fill tail. */
    .boot.stack ALIGN(16) : {
        __boot_stack_bottom = .;
        BYTE(0)
        . += 0x8000 - 1;
        __boot_stack_top = .;
    }
    __kernel_file_end = .;
    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __kernel_stack_bottom = .;
        . += 0x10000; /* 64 KiB primary stack for bounded DTB parsing. */
        __kernel_stack_top = .;
        . += 0x1000; /* Core 1 has its own privileged 4 KiB stack. */
        __core1_stack_top = .;
        . += 0x1000; /* Core 2 has its own privileged 4 KiB stack. */
        __core2_stack_top = .;
        . += 0x1000; /* Core 3 has its own privileged 4 KiB stack. */
        __core3_stack_top = .;
    }
    /* MMU-off workers and the cached primary only share this Device block
     * during the transition to the common translation tables. */
    .smp.boot ALIGN(0x200000) (NOLOAD) : {
        __smp_boot_start = .;
        *(.smp.boot)
        . = ALIGN(0x200000);
        __smp_boot_end = .;
    }
    /* Boot clears kernel BSS, padding and the boot gate together. */
    __bss_end = .;
    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.gnu)
        *(.note)
        *(.eh_frame*)
    }
}

__bss_size = (__bss_end - __bss_start)>>3;
