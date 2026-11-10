/* EL0 program entry. The kernel supplies the stack; returning from the
 * application exits the process. */
.section ".text.start", "ax"
.global _start
.type _start, %function
_start:
    mov x29, #0
    bl user_task
    mov x0, #0
1:
    mov x8, #18 /* SYS_TASK_EXIT */
    svc #0
    b 1b
.size _start, . - _start
