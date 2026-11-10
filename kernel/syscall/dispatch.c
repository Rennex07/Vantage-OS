#include <vantage/abi/syscall.h>
#include <vantage/abi/console.h>
#include <vantage/kernel/syscall.h>
#include <vantage/kernel/canvas.h>
#include <vantage/kernel/platform.h>
#include <vantage/kernel/files.h>
#include <vantage/drivers/key.h>
#include <vantage/drivers/touch.h>

#ifndef VANTAGE_SIM
#include <vantage/platform/pi4/io.h>
#include <vantage/platform/pi4/drivers.h>
#include <vantage/kernel/memory.h>
#include <vantage/kernel/task.h>
#endif

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
        const uint64_t owner = task_current();
#endif
        return (uint64_t)handle_file_call(syscall_id, (uintptr_t)arg0, owner);
    }
    if (syscall_id == SYS_TOUCH_READ) {
        if (arg0 % sizeof(uint32_t) ||
            !user_buffer_valid((uintptr_t)arg0, sizeof(TouchEvent))) return 0;
        fb_wait_frame();
        return touch_read((TouchEvent*)arg0);
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
        if (arg0 % sizeof(uint32_t) ||
            !user_buffer_valid((uintptr_t)arg0, sizeof(struct circle_args))) return 0;
        const struct circle_args args = *(const struct circle_args*)arg0;
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
        if (arg0 % sizeof(uint32_t) || !user_buffer_valid((uintptr_t)arg0, sizeof(KeyEvent))) return 0;
        return key_read((KeyEvent*)(uintptr_t)arg0);
    }
    else if (syscall_id == SYS_CANVAS_TEXT_ROW) {
        if (arg0 % sizeof(uint32_t) || !user_buffer_valid((uintptr_t)arg0, sizeof(TextRow))) return 0;
        const TextRow row = *(const TextRow*)(uintptr_t)arg0;
        return canvas_text_row(&row);
    }
    else if (syscall_id == SYS_CONSOLE_INFO) {
        ConsoleInfo info = {0};
        if (arg0 % sizeof(uint64_t) || !user_buffer_valid((uintptr_t)arg0, sizeof(info))) return 0;
        info.columns = canvas_text_columns();
        info.rows = canvas_text_rows();
#ifdef VANTAGE_SIM
        info.memory_kind = CONSOLE_MEMORY_HOST;
        info.host_backbuffer_bytes = sim_backbuffer_bytes();
#else
        if (mm_stats(&info.memory)) info.memory_kind = CONSOLE_MEMORY_KERNEL;
#endif
        *(ConsoleInfo*)(uintptr_t)arg0 = info;
        return 1;
    }
    else if (syscall_id == SYS_FRAME_WAIT) {
        fb_wait_frame();
#ifndef VANTAGE_SIM
        pi4_drivers_poll();
        task_yield();
#endif
    }
    else if (syscall_id == SYS_CONSOLE_WRITE) {
        if (arg0 % sizeof(uint32_t) || !user_buffer_valid((uintptr_t)arg0, sizeof(ConsoleWrite))) return 0;
        const ConsoleWrite request = *(const ConsoleWrite*)(uintptr_t)arg0;
        if (request.length > CONSOLE_WRITE_MAX) return 0;
#ifndef VANTAGE_SIM
        uart_write_bytes(request.text, request.length);
#endif
        return request.length;
    }

    else if (syscall_id == SYS_CANVAS_BUTTON) {
        if (arg0 % sizeof(uint32_t) || !user_buffer_valid((uintptr_t)arg0, sizeof(CanvasButton))) return 0;
        const CanvasButton button = *(const CanvasButton*)(uintptr_t)arg0;
        return canvas_button(&button);
    }
    else if (syscall_id == SYS_TIME_US) return time_us();

    return 0;
}
