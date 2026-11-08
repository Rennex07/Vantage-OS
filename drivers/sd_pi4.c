#include "../includes/sd.h"
#include "../includes/task.h"
#include "../kernel/sched/delays/delay.h"

#define EMMC2_BASE 0xfe340000ULL

static SdCard boot_card;

static int owner(void) {
    uint64_t core, level, masks;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(core));
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(level));
    __asm__ volatile("mrs %0, daif" : "=r"(masks));
    return !(core & 0x000000ff00ffffffULL) && level == 4 &&
           (masks & 0x3c0) == 0x3c0 && task_services_allowed();
}

static uint32_t host_read(void* context, uint32_t offset) {
    uint32_t value;
    (void)context;
    __asm__ volatile("dsb sy" ::: "memory");
    value = *(volatile uint32_t*)(uintptr_t)(EMMC2_BASE + offset);
    __asm__ volatile("dsb sy" ::: "memory");
    return value;
}

static void host_write(void* context, uint32_t offset, uint32_t value) {
    (void)context;
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t*)(uintptr_t)(EMMC2_BASE + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t host_time(void* context) { (void)context; return get_sys_timer(); }
static void host_delay(void* context, uint32_t microseconds) { (void)context; wait_usec(microseconds); }

int sd_init(uint32_t base_clock_hz) {
    const SdIo io = {NULL, host_read, host_write, host_time, host_delay};
    if (!owner()) return SD_INVALID;
    return sd_card_init(&boot_card, &io, base_clock_hz);
}

int sd_read_blocks(uint64_t lba, uint32_t count, void* buffer, size_t buffer_bytes) {
    if (!owner()) return SD_INVALID;
    return sd_card_read(&boot_card, lba, count, buffer, buffer_bytes);
}

int sd_write_blocks(uint64_t lba, uint32_t count, const void* buffer, size_t buffer_bytes) {
    if (!owner()) return SD_INVALID;
    return sd_card_write(&boot_card, lba, count, buffer, buffer_bytes);
}

int sd_sync(void) { return owner() ? sd_card_sync(&boot_card) : SD_INVALID; }
int sd_erase_blocks(uint64_t lba, uint32_t count) {
    return owner() ? sd_card_erase(&boot_card, lba, count) : SD_INVALID;
}
uint64_t sd_sector_count(void) { return owner() && boot_card.ready ? boot_card.sectors : 0; }

int sd_readBlock(unsigned int lba, unsigned char* buffer, unsigned int count) {
    if (!count || count > SD_READ_MAX_BLOCKS) return 0;
    return sd_read_blocks(lba, count, buffer, (size_t)count * SD_BLOCK_SIZE) == SD_OK ?
           (int)(count * SD_BLOCK_SIZE) : 0;
}
