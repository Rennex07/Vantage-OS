#include <vantage/drivers/hardware.h>
#include <vantage/platform/pi4/drivers.h>
#include <vantage/arch/mmu.h>

static PropertyMailbox mailbox;

static uint32_t read32(void* context, uintptr_t address) {
    (void)context;
    uint32_t value = *(volatile uint32_t*)address;
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static void write32(void* context, uintptr_t address, uint32_t value) {
    (void)context;
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t*)address = value;
}

static uint32_t clock_us(void* context) { return read32(context, 0xfe003004); }

static int owned(void* context) {
    (void)context;
    uint64_t core, level, mask;
    __asm__ volatile("mrs %0, mpidr_el1\nmrs %1, CurrentEL\nmrs %2, daif"
                     : "=r"(core), "=r"(level), "=r"(mask));
    return !(core & 0xff) && level == 4 && (mask & 0x3c0) == 0x3c0 && mmu_is_enabled();
}

static void cache(void* context, void* memory, size_t bytes, enum HwCache operation) {
    (void)context;
    uintptr_t end = (uintptr_t)memory + bytes;
    for (uintptr_t address = (uintptr_t)memory & ~63ULL; address < end; address += 64) {
        if (operation == HW_CLEAN) __asm__ volatile("dc cvac, %0" :: "r"(address) : "memory");
        else if (operation == HW_INVALIDATE) __asm__ volatile("dc ivac, %0" :: "r"(address) : "memory");
        else __asm__ volatile("dc civac, %0" :: "r"(address) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t physical(void* context, const void* memory) {
    (void)context;
    return (uintptr_t)memory; /* Immutable CPU identity mapping. */
}

static int property(void* context, uint32_t tag, uint32_t* values, unsigned int count) {
    (void)context;
    return property_call(&mailbox, pi4_hardware_ops(), tag, values, count);
}

const HwOps* pi4_hardware_ops(void) {
    static const HwOps operations = {NULL, read32, write32, clock_us, owned, cache, physical, property};
    return &operations;
}
