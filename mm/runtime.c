#include "internal.h"
#include "../includes/memory.h"
#include "../includes/io.h"

#define PAGE_COUNT ((size_t)(MM_RAM_LIMIT / MM_PAGE_SIZE))
#define HEAP_BYTES (8ULL * 1024 * 1024)
#define HEAP_SLOTS 1024
#define PHASE_FAILED 4

static MmDiscovery discovery;
static MmRange available_storage[MM_MAX_RANGES];
static MmRanges available;
static MmPages pages;
static uint32_t page_states[PAGE_COUNT];
static MmHeap heap;
static MmRange heap_free[HEAP_SLOTS], heap_live[HEAP_SLOTS];
static MmRange kernel_range, dtb_range, framebuffer_range, heap_range;
static uint32_t phase;
static volatile uint32_t busy;

static int owner(void) {
    uint64_t id, level, daif;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(id));
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(level));
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    return !(id & 0x000000ff00ffffffULL) && level == 4 && (daif & 0x3c0) == 0x3c0;
}

static uint64_t cache_state(void) {
    uint64_t control;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(control));
    return control & 5;
}

static int enter(void) {
    if (!owner() || busy) return 0;
    /* Core 0 is the sole owner and exceptions are masked. Plain accesses avoid
     * exclusive instructions against Device memory during MMU-off startup.
     * This guard is NOT a multicore lock. No metadata reference escapes a call. */
    busy = 1;
    __asm__ volatile("" ::: "memory");
    return 1;
}

static uint32_t leave(uint32_t result) {
    __asm__ volatile("" ::: "memory");
    busy = 0;
    return result;
}

uint32_t mm_prepare(uintptr_t dtb, uintptr_t kernel_end) {
    size_t size;
    uint32_t result = 0;
    MmRange header;
    if (!enter()) return 0;
    if (phase || cache_state()) return leave(0);
    phase = PHASE_FAILED;
    if (dtb < MM_PAGE_SIZE || (dtb & 7) || dtb > MM_RAM_LIMIT - 40 ||
        !kernel_end || kernel_end > MM_RAM_LIMIT) return leave(0);
    if (!mm_range_make(0, kernel_end, &kernel_range) || !mm_range_make(dtb, 40, &header) ||
        mm_range_intersects(kernel_range, header)) return leave(0);
    if (!mm_dtb_size((const uint8_t*)dtb, 40, &size) || !mm_range_make(dtb, size, &dtb_range) ||
        dtb_range.end > MM_RAM_LIMIT || mm_range_intersects(kernel_range, dtb_range)) return leave(0);
    mm_discovery_init(&discovery);
    if (mm_discover((const uint8_t*)dtb, size, &discovery) &&
        mm_ranges_contains(&discovery.ram, kernel_range) && mm_ranges_contains(&discovery.ram, dtb_range) &&
        mm_ranges_insert(&discovery.reserved, kernel_range) && mm_ranges_insert(&discovery.reserved, dtb_range)) {
        available.entries = available_storage;
        available.capacity = MM_MAX_RANGES;
        phase = 1;
        result = 1;
    }
    return leave(result);
}

uint32_t mm_finalize_framebuffer(uintptr_t address, uint64_t size) {
    MmRange framebuffer, range;
    size_t i;
    if (!enter()) return 0;
    if (phase != 1 || cache_state()) return leave(0);
    phase = PHASE_FAILED;
    if (!mm_range_make(address, size, &framebuffer) ||
        !mm_range_outward(framebuffer, MM_BLOCK_SIZE, &framebuffer) || framebuffer.end > MM_RAM_LIMIT ||
        mm_range_intersects(framebuffer, kernel_range) || mm_range_intersects(framebuffer, dtb_range) ||
        !mm_ranges_insert(&discovery.reserved, framebuffer) || !mm_discovery_finish(&discovery, &available)) return leave(0);
    for (i = 0; i < discovery.no_map.count; i++) {
        if (!mm_range_outward(discovery.no_map.entries[i], MM_BLOCK_SIZE, &range) ||
            mm_range_intersects(range, kernel_range) || mm_range_intersects(range, dtb_range) ||
            mm_range_intersects(range, framebuffer)) return leave(0);
    }
    framebuffer_range = framebuffer;
    phase = 2;
    return leave(1);
}

uint32_t mm_block_mapped(uint64_t address) {
    MmRange block;
    uint32_t result;
    if (!enter()) return 0;
    if (phase != 2 || address % MM_BLOCK_SIZE || address >= 0x100000000ULL) return leave(0);
    block.start = address;
    block.end = address + MM_BLOCK_SIZE;
    result = !mm_ranges_intersects(&discovery.no_map, block) &&
        (address >= MM_RAM_LIMIT || mm_ranges_intersects(&discovery.ram, block) ||
         mm_range_intersects(framebuffer_range, block));
    return leave(result);
}

uint32_t mm_init_allocator(void) {
    uint64_t start;
    if (!enter()) return 0;
    if (phase != 2 || cache_state() != 5) return leave(0);
    phase = PHASE_FAILED;
    if (!mm_pages_init(&pages, page_states, PAGE_COUNT, available.entries, available.count) ||
        !mm_pages_alloc(&pages, HEAP_BYTES / MM_PAGE_SIZE, 1, &start) ||
        !mm_range_make(start, HEAP_BYTES, &heap_range) ||
        !mm_heap_init(&heap, heap_range, heap_free, heap_live, HEAP_SLOTS)) return leave(0);
    phase = 3;
    return leave(1);
}

uint64_t mm_alloc_pages(uint64_t count, uint64_t alignment_pages) {
    uint64_t address = 0;
    if (!enter()) return 0;
    if (phase == 3 && cache_state() == 5) mm_pages_alloc(&pages, (size_t)count, (size_t)alignment_pages, &address);
    leave(0);
    return address;
}

uint32_t mm_free_pages(uint64_t address, uint64_t count) {
    MmRange range;
    uint32_t result = 0;
    if (!enter()) return 0;
    if (phase == 3 && cache_state() == 5 && count <= ~(uint64_t)0 / MM_PAGE_SIZE &&
        mm_range_make(address, count * MM_PAGE_SIZE, &range) && !mm_range_intersects(range, heap_range)) {
        result = mm_pages_release(&pages, address, (size_t)count);
    }
    return leave(result);
}

static void* allocate(uint64_t size, uint64_t alignment, int zero) {
    uint64_t address = 0;
    if (!enter()) return NULL;
    if (phase == 3 && cache_state() == 5 && mm_heap_alloc(&heap, size, alignment, &address) && zero) {
        mm_bytes_zero((void*)(uintptr_t)address, (size_t)size);
    }
    leave(0);
    return (void*)(uintptr_t)address;
}

void* mm_alloc(uint64_t size, uint64_t alignment) { return allocate(size, alignment, 0); }
void* mm_zalloc(uint64_t size, uint64_t alignment) { return allocate(size, alignment, 1); }

uint32_t mm_free(void* address) {
    uint32_t result = 0;
    if (!enter()) return 0;
    if (phase == 3 && cache_state() == 5) result = mm_heap_release(&heap, (uintptr_t)address);
    return leave(result);
}

uint32_t mm_stats(MemoryStats* out) {
    if (!out || (uintptr_t)out % sizeof(uint64_t) || !enter()) return 0;
    if (phase != 3 || cache_state() != 5) return leave(0);
    out->ram_bytes = mm_ranges_bytes(&discovery.ram);
    out->allocatable_pages = pages.total;
    out->free_pages = pages.free;
    out->heap_bytes = HEAP_BYTES;
    out->heap_used_bytes = heap.used;
    return leave(1);
}

uint32_t mm_self_test(void) {
    uint64_t p, a, b, before, used;
    size_t i;
    const uint64_t offsets[] = {0, MM_PAGE_SIZE - 1, MM_PAGE_SIZE, 2 * MM_PAGE_SIZE - 1};
    MmRange range;
    if (!enter()) return 0;
    if (phase != 3 || cache_state() != 5) return leave(0);
    before = pages.free;
    used = heap.used;
    if (!mm_pages_alloc(&pages, 2, 2, &p) || !p || p % (2 * MM_PAGE_SIZE) ||
        !mm_range_make(p, 2 * MM_PAGE_SIZE, &range) || mm_ranges_intersects(&discovery.reserved, range)) return leave(0);
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        volatile uint8_t* byte = (volatile uint8_t*)(uintptr_t)(p + offsets[i]);
        *byte = 0xa5;
        if (*byte != 0xa5) return leave(0);
    }
    if (mm_pages_release(&pages, p, 1) || !mm_pages_release(&pages, p, 2) || mm_pages_release(&pages, p, 2) ||
        !mm_heap_alloc(&heap, 33, 64, &a) || !mm_heap_alloc(&heap, 6000, MM_PAGE_SIZE, &b) ||
        a % 64 || b % MM_PAGE_SIZE || (a < b + 6000 && b < a + 33)) return leave(0);
    *(volatile uint8_t*)(uintptr_t)a = 0x5a;
    *(volatile uint8_t*)(uintptr_t)(b + 5999) = 0x3c;
    if (*(volatile uint8_t*)(uintptr_t)a != 0x5a || *(volatile uint8_t*)(uintptr_t)(b + 5999) != 0x3c ||
        !mm_heap_release(&heap, a) || !mm_heap_release(&heap, b) || pages.free != before || heap.used != used) return leave(0);
    return leave(1);
}

void mm_halt(const char* message) {
    uart_writeText(message);
    for (;;) __asm__ volatile("wfe");
}
