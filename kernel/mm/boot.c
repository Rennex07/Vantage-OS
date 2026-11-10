#include "internal.h"

uint32_t mm_boot_validate(uintptr_t dtb, uintptr_t clear_start, uintptr_t clear_end) {
    MmDiscovery info;
    MmRange clear = {clear_start, clear_end};
    MmRange kernel = {0, clear_end};
    MmRange tree;
    size_t size;

    /* Assembly checks these bounds before using the bootstrap stack.
     * Repeat them here so this helper has a complete numerical input contract. */
    if (clear_start >= clear_end || clear_end > MM_RAM_LIMIT || dtb < clear_end ||
        (dtb & 7) || dtb > MM_RAM_LIMIT - 40 ||
        !mm_dtb_size((const uint8_t*)dtb, 40, &size) ||
        !mm_range_make(dtb, size, &tree) || tree.end > MM_RAM_LIMIT) return 0;

    mm_discovery_init(&info);
    if (!mm_discover((const uint8_t*)dtb, size, &info) ||
        !mm_ranges_contains(&info.ram, kernel) ||
        !mm_ranges_contains(&info.ram, tree)) return 0;

    /* Includes memreserve entries, static reserved-memory nodes and the initrd.
     * Dynamic pools have no contents/address yet and are placed later, after
     * the kernel and firmware framebuffer have been reserved. */
    return !mm_ranges_intersects(&info.reserved, clear);
}
