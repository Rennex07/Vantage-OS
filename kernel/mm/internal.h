#pragma once
#include "pages.h"

#define MM_BLOCK_SIZE 0x200000ULL
#define MM_RAM_LIMIT 0xfc000000ULL
#define MM_MAX_DTB_SIZE (2U * 1024U * 1024U)
#define MM_MAX_RANGES 128
#define MM_MAX_DYNAMIC 16

/* All metadata is owned by the caller. These helpers do not access physical RAM. */
typedef struct {
    MmRange* entries;
    size_t count;
    size_t capacity;
} MmRanges;

int mm_align_up(uint64_t value, uint64_t alignment, uint64_t* result);
int mm_range_make(uint64_t start, uint64_t size, MmRange* result);
int mm_range_outward(MmRange range, uint64_t alignment, MmRange* result);
int mm_range_intersects(MmRange a, MmRange b);
int mm_ranges_contains(const MmRanges* ranges, MmRange range);
int mm_ranges_intersects(const MmRanges* ranges, MmRange range);
uint64_t mm_ranges_bytes(const MmRanges* ranges);
int mm_ranges_insert(MmRanges* ranges, MmRange range);
int mm_ranges_remove(MmRanges* ranges, MmRange range);
int mm_ranges_alloc(MmRanges* ranges, uint64_t size, uint64_t alignment,
                    MmRange within, uint64_t* address);
void mm_bytes_zero(void* pointer, size_t size);

typedef struct {
    uint64_t size;
    uint64_t alignment;
    MmRange allowed[8];
    size_t allowed_count;
    int no_map;
} MmDynamic;

typedef struct {
    MmRanges ram;
    MmRanges reserved;
    MmRanges no_map;
    MmRange ram_storage[MM_MAX_RANGES];
    MmRange reserved_storage[MM_MAX_RANGES];
    MmRange no_map_storage[MM_MAX_RANGES];
    MmDynamic dynamic[MM_MAX_DYNAMIC];
    size_t dynamic_count;
    size_t dtb_size;
} MmDiscovery;

void mm_discovery_init(MmDiscovery* info);
int mm_dtb_size(const uint8_t* header, size_t length, size_t* result);
/* On failure the output is incomplete and must not be used. One shot. */
int mm_discover(const uint8_t* blob, size_t length, MmDiscovery* info);
int mm_discovery_finish(MmDiscovery* info, MmRanges* available);

/* Before BSS initialization. Uses only the caller's validated, file-backed
 * bootstrap stack and read-only DTB/image data; no runtime globals. */
uint32_t mm_boot_validate(uintptr_t dtb, uintptr_t clear_start, uintptr_t clear_end);

typedef struct {
    MmRanges free;
    MmRange* live;
    size_t live_capacity;
    size_t live_count;
    MmRange arena;
    uint64_t used;
} MmHeap;

int mm_heap_init(MmHeap* heap, MmRange arena, MmRange* free_storage,
                 MmRange* live_storage, size_t capacity);
int mm_heap_alloc(MmHeap* heap, uint64_t size, uint64_t alignment, uint64_t* address);
int mm_heap_release(MmHeap* heap, uint64_t address);
