#pragma once
#include <vantage/abi/files.h>
#include <vantage/abi/task.h>

/* Stable syscall numbers shared by the EL0 library and EL1 dispatcher. */
enum {
    SYS_TOUCH_READ = 1,
    SYS_CANVAS_SET_COLOR = 2,
    SYS_CANVAS_CLEAR = 3,
    SYS_CANVAS_FILL_CIRCLE = 4,
    SYS_CANVAS_PRESENT = 5,
    SYS_KEY_READ = 6,
    SYS_CANVAS_TEXT_ROW = 7,
    SYS_CONSOLE_INFO = 8,
    SYS_FRAME_WAIT = 9,
    SYS_CONSOLE_WRITE = 10,
    SYS_CANVAS_BUTTON = 11,
    SYS_TIME_US = 12
};
