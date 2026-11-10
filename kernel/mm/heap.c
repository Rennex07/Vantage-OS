#include "internal.h"

int mm_heap_init(MmHeap* heap, MmRange arena, MmRange* free_storage,
                 MmRange* live_storage, size_t capacity) {
    if (!heap || heap->arena.end || !arena.start || arena.start >= arena.end ||
        !free_storage || !live_storage || capacity < 3) return 0;
    heap->free.entries = free_storage;
    heap->free.count = 0;
    heap->free.capacity = capacity;
    heap->live = live_storage;
    heap->live_capacity = capacity;
    heap->live_count = 0;
    heap->used = 0;
    mm_bytes_zero(live_storage, capacity * sizeof(*live_storage));
    if (!mm_ranges_insert(&heap->free, arena)) return 0;
    heap->arena = arena;
    return 1;
}

int mm_heap_alloc(MmHeap* heap, uint64_t size, uint64_t alignment, uint64_t* address) {
    size_t slot;
    uint64_t start;
    if (!heap || !heap->arena.end || !address) return 0;
    /* Reserve enough free-extent slots that releasing any live allocation succeeds. */
    if (heap->free.count + heap->live_count + 2 > heap->free.capacity) return 0;
    for (slot = 0; slot < heap->live_capacity && heap->live[slot].end; slot++) {}
    if (slot == heap->live_capacity ||
        !mm_ranges_alloc(&heap->free, size, alignment, heap->arena, &start)) return 0;
    heap->live[slot].start = start;
    heap->live[slot].end = start + size; /* Checked by mm_ranges_alloc. */
    heap->used += size; /* Disjoint live ranges are bounded by the arena. */
    heap->live_count++;
    *address = start;
    return 1;
}

int mm_heap_release(MmHeap* heap, uint64_t address) {
    size_t slot;
    if (!heap || !heap->arena.end) return 0;
    for (slot = 0; slot < heap->live_capacity; slot++) {
        MmRange range = heap->live[slot];
        if (range.end && range.start == address) {
            if (!mm_ranges_insert(&heap->free, range)) return 0;
            heap->used -= range.end - range.start;
            heap->live[slot].start = heap->live[slot].end = 0;
            heap->live_count--;
            return 1;
        }
    }
    return 0;
}
