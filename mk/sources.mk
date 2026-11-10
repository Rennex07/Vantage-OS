# Explicit production manifest: adding a source file does not silently link it.
# Legacy examples and host test doubles must never enter the kernel image.
ARCH_C_SOURCES := \
    arch/armv8/irq.c \
    arch/armv8/mmu.c \
    arch/armv8/mmu_tables.c \
    arch/armv8/parallel_port.c \
    arch/armv8/task_port.c \
    arch/armv8/trap.c

ARCH_ASM_SOURCES := \
    arch/armv8/boot.s \
    arch/armv8/exception.s \
    arch/armv8/secondary.s \
    arch/armv8/task_context.s

MEMORY_SOURCES := \
    kernel/mm/boot.c \
    kernel/mm/fdt.c \
    kernel/mm/heap.c \
    kernel/mm/pages.c \
    kernel/mm/ranges.c \
    kernel/mm/runtime.c

FILESYSTEM_SOURCES := \
    kernel/fs/file_syscall.c \
    kernel/fs/files.c \
    kernel/fs/storage.c

SCHEDULER_SOURCES := \
    kernel/parallel/worker.c \
    kernel/sched/self_test.c \
    kernel/sched/task.c

UI_SOURCES := \
    kernel/ui/canvas.c \
    kernel/ui/raster.c \
    kernel/ui/window_manager.c

DRIVER_SOURCES := \
    drivers/hw/property.c \
    drivers/hw/wait.c \
    drivers/input/key.c \
    drivers/input/touch.c \
    drivers/storage/sd.c \
    drivers/video/v3d.c \
    drivers/wifi/bus.c \
    drivers/wifi/cyw43455.c \
    drivers/wifi/nvram.c \
    drivers/wifi/sdio.c

PLATFORM_SOURCES := \
    platform/pi4/display.c \
    platform/pi4/hardware.c \
    platform/pi4/i2c.c \
    platform/pi4/io.c \
    platform/pi4/sd.c \
    platform/pi4/startup.c \
    platform/pi4/storage.c \
    platform/pi4/timer.c

USER_SOURCES := \
    user/apps/terminal/app.c \
    user/apps/terminal/terminal.c \
    user/lib/user_lib.c

C_SOURCES := $(sort $(ARCH_C_SOURCES) $(MEMORY_SOURCES) $(FILESYSTEM_SOURCES) \
    $(SCHEDULER_SOURCES) $(UI_SOURCES) $(DRIVER_SOURCES) $(PLATFORM_SOURCES) \
    $(USER_SOURCES) kernel/boot/main.c kernel/syscall/dispatch.c lib/memory.c)
S_SOURCES := $(ARCH_ASM_SOURCES)
