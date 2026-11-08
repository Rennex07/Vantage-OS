#include "../../mm/pages.h"
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed at line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void) {
    MmPages pages = {0};
    uint32_t states[128];
    MmRange ranges[] = {{4097, 9 * MM_PAGE_SIZE - 1}, {12 * MM_PAGE_SIZE, 128 * MM_PAGE_SIZE}};
    uint64_t p = 0, q = 0;
    CHECK(mm_pages_init(&pages, states, 128, ranges, 2));
    CHECK(pages.free == 122);
    CHECK(!mm_pages_init(&pages, states, 128, ranges, 2));
    CHECK(!mm_pages_alloc(&pages, 0, 1, &p));
    CHECK(!mm_pages_alloc(&pages, 1, 3, &p));
    CHECK(mm_pages_alloc(&pages, 4, 4, &p) && p == 4 * MM_PAGE_SIZE);
    CHECK(!mm_pages_release(&pages, p, 3));
    CHECK(!mm_pages_release(&pages, p + MM_PAGE_SIZE, 3));
    CHECK(!mm_pages_release(&pages, p + 1, 4));
    CHECK(mm_pages_alloc(&pages, 1, 1, &q) && q == 2 * MM_PAGE_SIZE);
    CHECK(mm_pages_release(&pages, p, 4));
    CHECK(!mm_pages_release(&pages, p, 4));
    CHECK(mm_pages_release(&pages, q, 1));
    CHECK(pages.free == pages.total);
    return 0;
}
