#include "../../includes/mmu.h"
#include "../../includes/memory.h"

#define MAIR_VALUE 0x44FF00ULL
#define TCR_VALUE (32ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (1ULL << 23) | (2ULL << 32))

static uint64_t l1[4] __attribute__((aligned(4096)));
static uint64_t l2[4][512] __attribute__((aligned(4096)));

extern unsigned char __user_start[];
extern unsigned char __user_end[];
extern unsigned char __smp_boot_start[];
extern volatile uint32_t secondary_mmu_ready;

uint32_t user_buffer_valid(uintptr_t buffer, size_t size) {
    return user_region_contains((uintptr_t)__user_start, (uintptr_t)__user_end,
                                buffer, size);
}

uint32_t mmu_is_enabled(void) {
    uint64_t control;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(control));
    return (control & 5) == 5;
}

void mmu_init(uintptr_t fbStart, uint32_t fbSize) {
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    if (((el >> 2) & 3) != 1) {
        mm_halt("[ERR] MMU initialization requires EL1\n");
    }
    uint64_t control;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(control));
    if (control & 5) mm_halt("[ERR] Live MMU tables cannot be rebuilt\n");

    mmu_populate_tables(l1, l2, fbStart, fbSize,
                        (uintptr_t)__user_start, (uintptr_t)__user_end,
                        (uintptr_t)__smp_boot_start);
    /* The tables are populated with caches off and remain immutable. */
    __asm__ volatile("dsb sy" ::: "memory");
    mmu_enable_shared();

    /* This block is Device memory on both sides of the MMU transition. */
    secondary_mmu_ready = 1;
    __asm__ volatile("dsb sy\nsev" ::: "memory");
}

void mmu_enable_shared(void) {
    uint64_t mair = MAIR_VALUE;
    uint64_t tcr = TCR_VALUE;
    uint64_t ttbr = (uint64_t)(uintptr_t)l1;

    __asm__ volatile("msr mair_el1, %0" :: "r"(mair) : "memory");
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("ic iallu" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12);
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr) : "memory");
    __asm__ volatile("isb" ::: "memory");
}

// Man I'm dead T-T
