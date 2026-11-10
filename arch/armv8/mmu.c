#include <vantage/arch/mmu.h>
#include <vantage/kernel/memory.h>

#define MAIR_VALUE 0x44FF00ULL
/* 39-bit TTBR0 space (T0SZ 25) with 8-bit ASIDs from TTBR0; TTBR1 disabled.
 * The low 4 GiB is the kernel identity map, user memory lies above it. */
#define TCR_VALUE (25ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (1ULL << 23) | (2ULL << 32))

static uint64_t l1[MMU_TABLE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t l2[MMU_KERNEL_ENTRIES][512] __attribute__((aligned(4096)));

extern unsigned char __smp_boot_start[];
extern volatile uint32_t secondary_mmu_ready;

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

    mmu_populate_tables(l1, l2, fbStart, fbSize, (uintptr_t)__smp_boot_start);
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

const uint64_t* mmu_kernel_level1(void) { return l1; }

void mmu_install_space(uint64_t space) {
    /* Kernel mappings are global and identical under both roots, so the
     * running code and stack stay mapped across the change. */
    uint64_t ttbr = space ? space : (uint64_t)(uintptr_t)l1;
    __asm__ volatile("msr ttbr0_el1, %0\nisb" :: "r"(ttbr) : "memory");
}

void mmu_publish_tables(void) {
    __asm__ volatile("dsb ishst\nisb" ::: "memory");
}

void mmu_flush_asid(uint32_t asid) {
    uint64_t argument = (uint64_t)asid << 48;
    __asm__ volatile("dsb ishst\ntlbi aside1is, %0\ndsb ish\nisb" :: "r"(argument) : "memory");
}

void mmu_clean_code(uintptr_t start, size_t size) {
    uint64_t type;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(type));
    uintptr_t line = (uintptr_t)4 << ((type >> 16) & 15);
    for (uintptr_t address = start & ~(line - 1); address < start + size; address += line)
        __asm__ volatile("dc cvau, %0" :: "r"(address) : "memory");
}

void mmu_invalidate_code(void) {
    /* The instruction cache may be indexed by the user virtual address. */
    __asm__ volatile("dsb ish\nic ialluis\ndsb ish\nisb" ::: "memory");
}

// Man I'm dead T-T
