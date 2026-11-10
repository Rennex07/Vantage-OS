/* The separately linked terminal program, copied into its own address space
 * by process_create. The build supplies the directory of terminal.bin. */
.section ".rodata"
.balign 8
.global terminal_image
.global terminal_image_end
terminal_image:
    .incbin "terminal.bin"
terminal_image_end:
