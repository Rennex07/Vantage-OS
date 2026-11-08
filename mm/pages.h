#pragma once
#include "../includes/std.h"

/* Page metadata is caller-owned and must remain exclusively accessible. */
#define MM_PAGE_SIZE 4096ULL
#define MM_PAGE_RESERVED 0xffffffffU
#define MM_PAGE_TAIL 0xfffffffeU

typedef struct {
    uint64_t start;
    uint64_t end;
} MmRange;

typedef struct {
    uint32_t* states;
    size_t count;
    size_t free;
    size_t total;
    int initialized;
} MmPages;

/* The ranges must be sorted, disjoint, nonempty half-open byte intervals. */
int mm_pages_init(MmPages* pages, uint32_t* states, size_t count,
                  const MmRange* ranges, size_t range_count);
int mm_pages_alloc(MmPages* pages, size_t count, size_t alignment, uint64_t* address);
int mm_pages_release(MmPages* pages, uint64_t address, size_t count);
