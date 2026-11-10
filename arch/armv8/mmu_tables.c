#include <vantage/arch/mmu.h>
#include <vantage/kernel/memory.h>

#define ATTR_DEVICE 0
#define ATTR_NORMAL 1
#define ATTR_NONCACHED 2
#define PERIPHERAL_START 0xFC000000ULL
#define BLOCK_SIZE 0x200000ULL
#define AP_PRIVILEGED_READ_ONLY (2ULL << 6)
#define PXN (1ULL << 53)
#define UXN (1ULL << 54)

/* Every kernel mapping is global and privileged-only: AP[1] stays clear, so
 * EL0 has no access, and UXN is set on every block. */
void mmu_populate_tables(uint64_t level1[4], uint64_t level2[4][512],
                         uintptr_t fbStart, uint32_t fbSize,
                         uintptr_t bootStart) {
    uint64_t fbEnd = (uint64_t)fbStart + fbSize;
    uint64_t g, i;

    for (g = 0; g < 4; g++) {
        for (i = 0; i < 512; i++) {
            uint64_t addr = (g << 30) | (i << 21);
            uint64_t desc = addr | (1ULL << 10) | UXN | PXN | 1;

            if (!mm_block_mapped(addr)) {
                level2[g][i] = 0; /* RAM holes and coarse no-map reservations fault. */
                continue;
            } else if (addr == bootStart || addr >= PERIPHERAL_START) {
                desc |= (ATTR_DEVICE << 2) | (2ULL << 8);
            } else if (addr + BLOCK_SIZE > fbStart && addr < fbEnd) {
                desc |= (ATTR_NONCACHED << 2) | (2ULL << 8);
            } else if (addr == 0) {
                /* Kernel text and constants: read-only, executable at EL1 only. */
                desc = addr | (ATTR_NORMAL << 2) | (3ULL << 8) | (1ULL << 10) |
                       AP_PRIVILEGED_READ_ONLY | UXN | 1;
            } else {
                desc |= (ATTR_NORMAL << 2) | (3ULL << 8);
            }
            level2[g][i] = desc;
        }
        level1[g] = (uint64_t)(uintptr_t)level2[g] | 3;
    }
}
