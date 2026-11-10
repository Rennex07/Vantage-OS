#include <vantage/kernel/window_manager.h>
#include <vantage/arch/mmu.h>
#include <vantage/kernel/parallel.h>
#include <vantage/kernel/memory.h>
#include <vantage/kernel/task.h>
#include <vantage/platform/pi4/storage.h>
#include <vantage/platform/pi4/io.h>
#include <vantage/platform/pi4/drivers.h>

#if VANTAGE_STORAGE_VALIDATION && (SD_BASE_CLOCK_HZ < 400000 || SD_BASE_CLOCK_HZ > 818400000 || STORAGE_PARTITION > 3 || STORAGE_PARTITION < 0)
#error Invalid storage validation clock or primary partition
#endif

extern unsigned char _end[];
void user_task(void);
void enter_user_space(void (*func)(void), void* stack_top);
unsigned int secondary_core_start(unsigned int core_id);

static unsigned char user_stack[4096]
    __attribute__((section(".user.bss"), aligned(16)));

static void user_entry(void* argument) {
    (void)argument;
    enter_user_space(user_task, &user_stack[sizeof(user_stack)]);
}

void main(uintptr_t dtb) {
    uart_init();
    if (!mm_prepare(dtb, (uintptr_t)_end))
        mm_halt("[ERR] Firmware memory discovery failed; boot halted\n");
    uart_writeText("[OK] Firmware RAM and reservations discovered\n");
    for (unsigned int core = 1; core < 4; core++) {
        if (secondary_core_start(core)) {
            if (core == 1) uart_writeText("[OK] Core 1 entry reached\n");
            if (core == 2) uart_writeText("[OK] Core 2 entry reached\n");
            if (core == 3) uart_writeText("[OK] Core 3 entry reached\n");
        } else {
            if (core == 1) uart_writeText("[ERR] Core 1 startup unconfirmed; boot halted\n");
            if (core == 2) uart_writeText("[ERR] Core 2 startup unconfirmed; boot halted\n");
            if (core == 3) uart_writeText("[ERR] Core 3 startup unconfirmed; boot halted\n");
            for (;;) __asm__ volatile("wfe");
        }
    }
    env_Init(480, 800);
    if (!mmu_is_enabled() || !parallel_init() || !parallel_self_test())
        parallel_halt("[ERR] Four-core parallel initialization failed; boot halted\n");
    uart_writeText("[OK] Four-core parallel self-test passed\n");
    pi4_drivers_start();
    if (task_init() != TASK_OK || !task_self_test())
        mm_halt("[ERR] Cooperative task self-test failed; boot halted\n");
    uart_writeText("[OK] Cooperative task self-test passed\n");
    if (!task_multicore_self_test())
        mm_halt("[ERR] Four-core task scheduling self-test failed; boot halted\n");
    uart_writeText("[OK] Tasks executed concurrently on cores 0, 1, 2 and 3\n");

#if VANTAGE_STORAGE_VALIDATION
    /* Disposable validation image: mount a deliberately provisioned partition.
     * Never initialize SD in normal boot, format a volume or erase card data. */
    int storage_result = storage_start(SD_BASE_CLOCK_HZ, STORAGE_PARTITION);
    if (storage_result < 0) mm_halt("[ERR] Persistent storage mount failed; validation boot halted\n");
    if (storage_result == STORE_MOUNT_RECOVERED)
        uart_writeText("[WARN] Storage recovered one valid snapshot; inspect card and validation logs\n");
    uart_writeText("[OK] Persistent storage mounted in hardware-validation image\n");
    int probe = storage_reboot_probe();
    if (probe < 0) mm_halt("[ERR] Storage reboot probe failed; validation boot halted\n");
    if (probe) uart_writeText("[TEST] Stored reboot probe matched; retain separate cold-boot logs\n");
    else uart_writeText("[TEST] Storage reboot probe written; power-cycle this validation image to check persistence\n");
#else
    uart_writeText("[INFO] SD storage disabled pending physical read/write validation\n");
#endif

    uart_writeText("\n[OK] Vantage booted!...\n");

    TaskId app = task_create(user_entry, NULL);
    if (!app || task_run() != TASK_OK || task_reap(app, NULL) != TASK_OK)
        mm_halt("[ERR] Application scheduling failed; boot halted\n");

    for (;;) __asm__ volatile("wfe");
}
