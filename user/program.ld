/* Flat EL0 program image, loaded by process_create at USER_BASE. The header
 * is ProcessImage in include/vantage/kernel/process.h. Text, read-only data
 * and writable data each start on a page so they can have separate
 * permissions; bss is not part of the file. */
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0x100000000;
    .text : {
        QUAD(0x31676d4945544e56)
        QUAD(_start)
        QUAD(__text_bytes)
        QUAD(__rodata_bytes)
        QUAD(__data_bytes)
        QUAD(__bss_bytes)
        *(.text.start)
        *(.text .text.*)
        . = ALIGN(0x1000);
    } :text
    .rodata ALIGN(0x1000) : {
        *(.rodata .rodata.*)
        . = ALIGN(0x1000);
    } :rodata
    .data ALIGN(0x1000) : {
        *(.data .data.*)
        . = .;
    } :data
    .bss (NOLOAD) : {
        *(.bss .bss.*)
        *(COMMON)
        . = .;
    } :data
    __image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.gnu)
        *(.note)
        *(.eh_frame*)
    }
}

__text_bytes = SIZEOF(.text);
__rodata_bytes = SIZEOF(.rodata);
__data_bytes = SIZEOF(.data);
__bss_bytes = SIZEOF(.bss);
ASSERT(ADDR(.rodata) == ADDR(.text) + SIZEOF(.text), "rodata must follow text")
ASSERT(ADDR(.data) == ADDR(.rodata) + SIZEOF(.rodata), "data must follow rodata")
ASSERT(ADDR(.bss) == ADDR(.data) + SIZEOF(.data), "bss must follow data")
