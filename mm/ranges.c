#include "internal.h"

void mm_bytes_zero(void* pointer, size_t size) {
    uint8_t* bytes = pointer;
    size_t i;
    for (i = 0; i < size; i++) bytes[i] = 0;
}

int mm_align_up(uint64_t value, uint64_t alignment, uint64_t* result) {
    if (!alignment || (alignment & (alignment - 1)) ||
        value > ~(uint64_t)0 - (alignment - 1)) return 0;
    *result = (value + alignment - 1) & ~(alignment - 1);
    return 1;
}

int mm_range_make(uint64_t start, uint64_t size, MmRange* result) {
    if (!size || start > ~(uint64_t)0 - size) return 0;
    result->start = start;
    result->end = start + size;
    return 1;
}

int mm_range_outward(MmRange range, uint64_t alignment, MmRange* result) {
    uint64_t end;
    if (range.start >= range.end || !mm_align_up(range.end, alignment, &end)) return 0;
    result->start = range.start & ~(alignment - 1);
    result->end = end;
    return 1;
}

int mm_range_intersects(MmRange a, MmRange b) {
    return a.start < b.end && b.start < a.end;
}

int mm_ranges_contains(const MmRanges* ranges, MmRange range) {
    size_t i;
    for (i = 0; i < ranges->count; i++) {
        if (ranges->entries[i].start <= range.start && range.end <= ranges->entries[i].end) return 1;
    }
    return 0;
}

int mm_ranges_intersects(const MmRanges* ranges, MmRange range) {
    size_t i;
    for (i = 0; i < ranges->count; i++) {
        if (mm_range_intersects(ranges->entries[i], range)) return 1;
    }
    return 0;
}

uint64_t mm_ranges_bytes(const MmRanges* ranges) {
    uint64_t bytes = 0;
    size_t i;
    for (i = 0; i < ranges->count; i++) bytes += ranges->entries[i].end - ranges->entries[i].start;
    return bytes;
}

/* Sorted, coalesced half-open extents. Failure leaves the set unchanged. */
int mm_ranges_insert(MmRanges* ranges, MmRange range) {
    size_t first = 0, last, i;
    if (range.start >= range.end) return 0;
    while (first < ranges->count && ranges->entries[first].end < range.start) first++;
    last = first;
    while (last < ranges->count && ranges->entries[last].start <= range.end) {
        if (range.start > ranges->entries[last].start) range.start = ranges->entries[last].start;
        if (range.end < ranges->entries[last].end) range.end = ranges->entries[last].end;
        last++;
    }
    if (first == last) {
        if (ranges->count == ranges->capacity) return 0;
        for (i = ranges->count; i > first; i--) ranges->entries[i] = ranges->entries[i - 1];
        ranges->count++;
    } else {
        for (i = last; i < ranges->count; i++) ranges->entries[first + 1 + i - last] = ranges->entries[i];
        ranges->count -= last - first - 1;
    }
    ranges->entries[first] = range;
    return 1;
}

int mm_ranges_remove(MmRanges* ranges, MmRange range) {
    size_t i, j;
    if (range.start >= range.end) return 0;
    if (ranges->count == ranges->capacity) {
        for (i = 0; i < ranges->count; i++) {
            if (ranges->entries[i].start < range.start && range.end < ranges->entries[i].end) return 0;
        }
    }
    i = 0;
    while (i < ranges->count) {
        MmRange old = ranges->entries[i];
        if (!mm_range_intersects(old, range)) {
            i++;
        } else if (range.start <= old.start && old.end <= range.end) {
            for (j = i + 1; j < ranges->count; j++) ranges->entries[j - 1] = ranges->entries[j];
            ranges->count--;
        } else if (old.start < range.start && range.end < old.end) {
            for (j = ranges->count; j > i + 1; j--) ranges->entries[j] = ranges->entries[j - 1];
            ranges->entries[i].end = range.start;
            ranges->entries[i + 1].start = range.end;
            ranges->entries[i + 1].end = old.end;
            ranges->count++;
            break;
        } else {
            if (old.start < range.start) ranges->entries[i].end = range.start;
            else ranges->entries[i].start = range.end;
            i++;
        }
    }
    return 1;
}

int mm_ranges_alloc(MmRanges* ranges, uint64_t size, uint64_t alignment,
                    MmRange within, uint64_t* address) {
    size_t i;
    uint64_t start;
    MmRange allocation;
    if (!size || !alignment || (alignment & (alignment - 1)) || within.start >= within.end) return 0;
    for (i = 0; i < ranges->count; i++) {
        MmRange old = ranges->entries[i];
        uint64_t lower = old.start > within.start ? old.start : within.start;
        uint64_t upper = old.end < within.end ? old.end : within.end;
        if (!mm_align_up(lower, alignment, &start) || !mm_range_make(start, size, &allocation)) continue;
        if (allocation.end <= upper && mm_ranges_remove(ranges, allocation)) {
            *address = start;
            return 1;
        }
    }
    return 0;
}
