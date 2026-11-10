#include "kernel/mm/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static uint64_t seed;
static uint64_t random_value(void) {
    seed = seed * 6364136223846793005ULL + 1;
    return seed >> 24;
}

static void ranges_test(void) {
    MmRange storage[128], r;
    MmRanges set = {storage, 0, 128};
    unsigned char oracle[128] = {0};
    uint64_t n;
    size_t iteration, i;
    CHECK(!mm_align_up(1, 0, &n));
    CHECK(!mm_align_up(~(uint64_t)0, 4096, &n));
    CHECK(mm_align_up(4097, 4096, &n) && n == 8192);
    CHECK(!mm_range_make(~(uint64_t)0, 1, &r));
    CHECK(!mm_range_make(0, 0, &r));
    seed = 3;
    for (iteration = 0; iteration < 20000; iteration++) {
        size_t start = (size_t)(random_value() % 128);
        size_t end = start + 1 + (size_t)(random_value() % (128 - start));
        int insert = (random_value() & 1) == 0;
        size_t bytes = 0;
        r.start = start; r.end = end;
        CHECK(insert ? mm_ranges_insert(&set, r) : mm_ranges_remove(&set, r));
        memset(oracle + start, insert, end - start);
        for (i = 0; i < 128; i++) {
            MmRange byte = {i, i + 1};
            CHECK(mm_ranges_contains(&set, byte) == oracle[i]);
            bytes += oracle[i];
        }
        CHECK(mm_ranges_bytes(&set) == bytes);
        for (i = 1; i < set.count; i++) CHECK(set.entries[i - 1].end < set.entries[i].start);
    }
    set.count = 0; set.capacity = 2;
    r.start = 10; r.end = 20; CHECK(mm_ranges_insert(&set, r));
    r.start = 30; r.end = 40; CHECK(mm_ranges_insert(&set, r));
    r.start = 50; r.end = 60; CHECK(!mm_ranges_insert(&set, r));
    r.start = 12; r.end = 15; CHECK(!mm_ranges_remove(&set, r));
    CHECK(set.count == 2 && set.entries[0].start == 10 && set.entries[0].end == 20);
    r.start = 20; r.end = 30; CHECK(mm_ranges_insert(&set, r));
    CHECK(set.count == 1 && set.entries[0].start == 10 && set.entries[0].end == 40);
}

static void pages_test(void) {
    MmPages pages = {0}, invalid = {0};
    uint32_t states[128], bad_states[16];
    MmRange free[] = {{MM_PAGE_SIZE, 53 * MM_PAGE_SIZE}, {60 * MM_PAGE_SIZE, 128 * MM_PAGE_SIZE}};
    MmRange bad[] = {{MM_PAGE_SIZE, 3 * MM_PAGE_SIZE}, {~(uint64_t)0 - 10, ~(uint64_t)0}};
    unsigned char busy[128];
    struct {size_t start, count;} live[128];
    size_t live_count = 0, iteration, i;
    CHECK(!mm_pages_init(&invalid, bad_states, 16, bad, 2));
    CHECK(!invalid.free && !invalid.initialized);
    CHECK(mm_pages_init(&invalid, bad_states, 16, free, 1));
    CHECK(invalid.free == 15);
    CHECK(mm_pages_init(&pages, states, 128, free, 2));
    memset(busy, 1, sizeof(busy));
    memset(busy + 1, 0, 52);
    memset(busy + 60, 0, 68);
    seed = 7;
    for (iteration = 0; iteration < 20000; iteration++) {
        if (live_count && random_value() % 3 == 0) {
            size_t slot = (size_t)(random_value() % live_count);
            CHECK(mm_pages_release(&pages, (uint64_t)live[slot].start * MM_PAGE_SIZE, live[slot].count));
            memset(busy + live[slot].start, 0, live[slot].count);
            live[slot] = live[--live_count];
        } else {
            size_t count = 1 + (size_t)(random_value() % 12);
            size_t alignment = (size_t)1 << (random_value() % 5);
            size_t expected = 128, start;
            uint64_t address = 0;
            int success;
            for (start = 0; start <= 128 - count; start++) {
                if (start % alignment) continue;
                for (i = start; i < start + count && !busy[i]; i++) {}
                if (i == start + count) { expected = start; break; }
            }
            success = mm_pages_alloc(&pages, count, alignment, &address);
            CHECK(success == (expected != 128));
            if (success) {
                CHECK(address == (uint64_t)expected * MM_PAGE_SIZE);
                memset(busy + expected, 1, count);
                live[live_count].start = expected;
                live[live_count++].count = count;
            }
        }
        { size_t available = 0;
          for (i = 0; i < 128; i++) available += !busy[i];
          CHECK(pages.free == available); }
    }
    while (live_count) {
        live_count--;
        CHECK(mm_pages_release(&pages, (uint64_t)live[live_count].start * MM_PAGE_SIZE, live[live_count].count));
    }
    CHECK(pages.free == 120);
}

static void heap_test(void) {
    MmHeap heap = {0};
    MmRange free[256], live_storage[256];
    MmRange arena = {4096, 4096 + 65536};
    struct {uint64_t address, size;} live[256];
    size_t live_count = 0, iteration, i;
    uint64_t address, a, b;
    CHECK(mm_heap_init(&heap, arena, free, live_storage, 256));
    CHECK(!mm_heap_init(&heap, arena, free, live_storage, 256));
    CHECK(!mm_heap_alloc(&heap, 0, 1, &address));
    CHECK(!mm_heap_alloc(&heap, 1, 3, &address));
    CHECK(mm_heap_alloc(&heap, 31, 64, &a) && a % 64 == 0);
    CHECK(mm_heap_alloc(&heap, 65, 256, &b) && b % 256 == 0);
    CHECK(!mm_heap_release(&heap, a + 1));
    CHECK(!mm_heap_release(&heap, 0));
    CHECK(mm_heap_release(&heap, a));
    CHECK(!mm_heap_release(&heap, a));
    CHECK(mm_heap_release(&heap, b));
    seed = 11;
    for (iteration = 0; iteration < 20000; iteration++) {
        uint64_t used = 0;
        if (live_count && !(random_value() & 1)) {
            size_t slot = (size_t)(random_value() % live_count);
            CHECK(mm_heap_release(&heap, live[slot].address));
            live[slot] = live[--live_count];
        } else {
            uint64_t size = 1 + random_value() % 1024;
            uint64_t alignment = 1ULL << (random_value() % 13);
            if (mm_heap_alloc(&heap, size, alignment, &address)) {
                CHECK(address % alignment == 0 && address >= arena.start && address + size <= arena.end);
                for (i = 0; i < live_count; i++) {
                    CHECK(!(address < live[i].address + live[i].size && live[i].address < address + size));
                }
                CHECK(live_count < 256);
                live[live_count].address = address;
                live[live_count++].size = size;
            }
        }
        for (i = 0; i < live_count; i++) used += live[i].size;
        CHECK(heap.used == used);
    }
    while (live_count) CHECK(mm_heap_release(&heap, live[--live_count].address));
    CHECK(!heap.used);
    CHECK(mm_heap_alloc(&heap, 65536, 1, &address) && address == arena.start);
    CHECK(!mm_heap_alloc(&heap, 1, 1, &address));
    CHECK(mm_heap_release(&heap, arena.start));
    /* Force metadata exhaustion with many tiny aligned allocations. Every live
     * allocation must remain releasable even at the metadata limit. */
    while (mm_heap_alloc(&heap, 1, 16, &address)) {
        CHECK(live_count < 256);
        live[live_count++].address = address;
    }
    CHECK(live_count && live_count < 256);
    while (live_count) CHECK(mm_heap_release(&heap, live[--live_count].address));
    CHECK(mm_heap_alloc(&heap, 65536, 1, &address) && address == arena.start);
}

int main(void) {
    ranges_test();
    pages_test();
    heap_test();
    puts("PASS: ranges/page runs versus independent models; heap alignment, recovery and metadata pressure");
    return 0;
}
