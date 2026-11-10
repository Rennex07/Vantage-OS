#pragma once

#include <vantage/base.h>

/* Entries of one translation table; level 1 covers 512 GiB with T0SZ = 25. */
#define MMU_TABLE_ENTRIES 512U
/* The kernel identity map owns the first four level-1 entries (4 GiB). */
#define MMU_KERNEL_ENTRIES 4U

void mmu_init(uintptr_t fbStart, uint32_t fbSize);
void mmu_enable_shared(void);
uint32_t mmu_is_enabled(void);

void mmu_populate_tables(uint64_t level1[4], uint64_t level2[4][512],
                         uintptr_t fbStart, uint32_t fbSize,
                         uintptr_t bootStart);

/* Immutable after mmu_init: the kernel's level-1 entries, shared by every
 * address space so that kernel mappings are identical under any root. */
const uint64_t* mmu_kernel_level1(void);
/* Installs a user half (root | ASID << 48) on the calling core; 0 selects the
 * kernel-only root with ASID 0. EL1 with all DAIF masks set. */
void mmu_install_space(uint64_t space);
/* Orders new translation-table entries before their first use. */
void mmu_publish_tables(void);
/* Removes one ASID's cached translations on every core. */
void mmu_flush_asid(uint32_t asid);
/* Instructions written through the kernel's identity alias: clean them to the
 * point of unification, then drop stale instructions on every core for any
 * virtual address they may be fetched from. */
void mmu_clean_code(uintptr_t start, size_t size);
void mmu_invalidate_code(void);

/* Bounds helper for host backends that model one user buffer region. */
static inline uint32_t user_region_contains(uintptr_t start, uintptr_t end,
                                           uintptr_t buffer, size_t size) {
    return start < end && buffer >= start && buffer < end &&
           size <= end - buffer;
}
