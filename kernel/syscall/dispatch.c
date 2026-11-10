#include <vantage/abi/syscall.h>
#include <vantage/abi/console.h>
#include <vantage/kernel/syscall.h>
#include <vantage/kernel/canvas.h>
#include <vantage/kernel/platform.h>
#include <vantage/kernel/files.h>
#include <vantage/kernel/user_access.h>
#include <vantage/drivers/key.h>
#include <vantage/drivers/touch.h>

#ifndef VANTAGE_SIM
#include <vantage/platform/pi4/io.h>
#include <vantage/platform/pi4/drivers.h>
#include <vantage/kernel/memory.h>
#include <vantage/kernel/process.h>
#endif

/* Payloads are copied between the calling process and the kernel stack; no
 * service ever receives a user pointer. Alignment is part of the ABI. */
static uint32_t fetch(void* payload, uint64_t address, size_t size, unsigned int alignment) {
    return !(address % alignment) && user_copy_in(payload, (uintptr_t)address, size);
}

static uint32_t writable(uint64_t address, size_t size, unsigned int alignment) {
    return !(address % alignment) && user_access_ok((uintptr_t)address, size, 1);
}

uint64_t handle_sys_call(uint64_t syscall_id, uint64_t arg0) {
#ifndef VANTAGE_SIM
    /* Scheduling syscalls are available on all cores. Existing device/file
     * services remain exclusive to boot or immutable core-0-pinned tasks. */
    if (syscall_id == SYS_TASK_ID) return task_current();
    if (syscall_id == SYS_TASK_YIELD) return task_yield();
    if (syscall_id == SYS_TASK_EXIT) {
        if (task_current()) task_exit((int)(uint32_t)arg0);
        return 0;
    }
    if (!task_services_allowed()) {
        if (syscall_id >= SYS_FILE_OPEN && syscall_id <= SYS_FILE_REMOVE)
            return (uint64_t)(int64_t)FILE_UNAVAILABLE;
        return 0;
    }
#endif
    if (syscall_id >= SYS_FILE_OPEN && syscall_id <= SYS_FILE_REMOVE) {
#ifdef VANTAGE_SIM
        const uint64_t owner = 1; /* One host app, without emulated task contexts. */
#else
        /* Handles belong to the calling process, or to a kernel task. */
        const uint64_t owner = process_current() ? process_current() : task_current();
#endif
        return (uint64_t)handle_file_call(syscall_id, (uintptr_t)arg0, owner);
    }
    if (syscall_id == SYS_TOUCH_READ) {
        TouchEvent event;
        if (!writable(arg0, sizeof(event), sizeof(uint32_t))) return 0;
        fb_wait_frame();
        uint32_t result = touch_read(&event);
        return user_copy_out((uintptr_t)arg0, &event, sizeof(event)) ? result : 0;
    }
    else if (syscall_id == SYS_CANVAS_SET_COLOR) {
        // Set Color (arg0 is the color value)
        canvas_set_color((uint32_t)arg0);
    }
    else if (syscall_id == SYS_CANVAS_CLEAR) {
        // Clear Canvas
        canvas_clear();
    }
    else if (syscall_id == SYS_CANVAS_FILL_CIRCLE) {
        // Fill Circle (arg0 is pointer to struct circle_args)
        struct circle_args args;
        if (!fetch(&args, arg0, sizeof(args), sizeof(uint32_t))) return 0;
        canvas_fill_circle(args.x, args.y, args.r);
    }
    else if (syscall_id == SYS_CANVAS_PRESENT) {
        // Present Canvas
        canvas_present();
#ifndef VANTAGE_SIM
        pi4_drivers_poll();
        task_yield();
#endif
    }
    else if (syscall_id == SYS_KEY_READ) {
        KeyEvent event;
        if (!writable(arg0, sizeof(event), sizeof(uint32_t))) return 0;
        uint32_t result = key_read(&event);
        return user_copy_out((uintptr_t)arg0, &event, sizeof(event)) ? result : 0;
    }
    else if (syscall_id == SYS_CANVAS_TEXT_ROW) {
        TextRow row;
        if (!fetch(&row, arg0, sizeof(row), sizeof(uint32_t))) return 0;
        return canvas_text_row(&row);
    }
    else if (syscall_id == SYS_CONSOLE_INFO) {
        ConsoleInfo info = {0};
        if (!writable(arg0, sizeof(info), sizeof(uint64_t))) return 0;
        info.columns = canvas_text_columns();
        info.rows = canvas_text_rows();
#ifdef VANTAGE_SIM
        info.memory_kind = CONSOLE_MEMORY_HOST;
        info.host_backbuffer_bytes = sim_backbuffer_bytes();
#else
        if (mm_stats(&info.memory)) info.memory_kind = CONSOLE_MEMORY_KERNEL;
#endif
        return user_copy_out((uintptr_t)arg0, &info, sizeof(info));
    }
    else if (syscall_id == SYS_FRAME_WAIT) {
        fb_wait_frame();
#ifndef VANTAGE_SIM
        pi4_drivers_poll();
        task_yield();
#endif
    }
    else if (syscall_id == SYS_CONSOLE_WRITE) {
        ConsoleWrite request;
        if (!fetch(&request, arg0, sizeof(request), sizeof(uint32_t))) return 0;
        if (request.length > CONSOLE_WRITE_MAX) return 0;
#ifndef VANTAGE_SIM
        uart_write_bytes(request.text, request.length);
#endif
        return request.length;
    }

    else if (syscall_id == SYS_CANVAS_BUTTON) {
        CanvasButton button;
        if (!fetch(&button, arg0, sizeof(button), sizeof(uint32_t))) return 0;
        return canvas_button(&button);
    }
    else if (syscall_id == SYS_TIME_US) return time_us();

    return 0;
}
