#include "pages.h"

int mm_pages_init(MmPages* pages, uint32_t* states, size_t count,
                  const MmRange* ranges, size_t range_count) {
    size_t i, j, free_pages = 0;
    if (!pages || !states || !count || pages->initialized || (range_count && !ranges)) return 0;
    for (i = 0; i < range_count; i++) {
        if (ranges[i].start >= ranges[i].end ||
            ranges[i].start > ~(uint64_t)0 - (MM_PAGE_SIZE - 1) ||
            (i && ranges[i - 1].end > ranges[i].start)) return 0;
    }
    for (i = 0; i < count; i++) states[i] = MM_PAGE_RESERVED;
    for (i = 0; i < range_count; i++) {
        uint64_t first = (ranges[i].start + MM_PAGE_SIZE - 1) / MM_PAGE_SIZE;
        uint64_t end = ranges[i].end / MM_PAGE_SIZE;
        if (end > count) end = count;
        for (j = (size_t)(first < end ? first : end); j < (size_t)end; j++) {
            states[j] = 0;
            free_pages++;
        }
    }
    pages->states = states;
    pages->count = count;
    pages->free = free_pages;
    pages->total = free_pages;
    pages->initialized = 1;
    return 1;
}

int mm_pages_alloc(MmPages* pages, size_t count, size_t alignment, uint64_t* address) {
    size_t start = 0, offset, end;
    if (!pages || !pages->initialized || !address || !count || count >= MM_PAGE_TAIL ||
        !alignment || (alignment & (alignment - 1)) || count > pages->free || count > pages->count) return 0;
    while (start <= pages->count - count) {
        if (start > ~(size_t)0 - (alignment - 1)) return 0;
        start = (start + alignment - 1) & ~(alignment - 1);
        if (start > pages->count - count) break;
        end = start + count;
        for (offset = start; offset < end && pages->states[offset] == 0; offset++) {}
        if (offset == end) {
            pages->states[start] = (uint32_t)count;
            for (offset = start + 1; offset < end; offset++) pages->states[offset] = MM_PAGE_TAIL;
            pages->free -= count;
            *address = (uint64_t)start * MM_PAGE_SIZE;
            return 1;
        }
        start = offset + 1;
    }
    return 0;
}

int mm_pages_release(MmPages* pages, uint64_t address, size_t count) {
    uint64_t first = address / MM_PAGE_SIZE;
    size_t start, i;
    if (!pages || !pages->initialized || address % MM_PAGE_SIZE || !count ||
        count >= MM_PAGE_TAIL || first >= pages->count) return 0;
    start = (size_t)first;
    if (count > pages->count - start || pages->states[start] != count) return 0;
    for (i = start + 1; i < start + count; i++) if (pages->states[i] != MM_PAGE_TAIL) return 0;
    for (i = start; i < start + count; i++) pages->states[i] = 0;
    pages->free += count;
    return 1;
}
