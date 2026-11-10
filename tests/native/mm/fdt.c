#include "kernel/mm/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static uint8_t tree[65536], strings[65536], blob[150000], saved[150000];
static MmRange reservations[130];
static size_t tree_len, strings_len, reservation_count;
static MmDiscovery info;
static MmRange available_storage[MM_MAX_RANGES];
static MmRanges available;

static void put32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24); out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8); out[3] = (uint8_t)value;
}
static void put64(uint8_t* out, uint64_t value) { put32(out, (uint32_t)(value >> 32)); put32(out + 4, (uint32_t)value); }
static void word(uint32_t value) { CHECK(tree_len + 4 <= sizeof(tree)); put32(tree + tree_len, value); tree_len += 4; }
static void begin(const char* name) {
    size_t length = strlen(name) + 1;
    word(1);
    CHECK(tree_len + length + 3 <= sizeof(tree));
    memcpy(tree + tree_len, name, length); tree_len += length;
    while (tree_len & 3) tree[tree_len++] = 0;
}
static void end(void) { word(2); }
static void prop(const char* name, const void* bytes, size_t length) {
    size_t name_length = strlen(name) + 1;
    CHECK(strings_len + name_length <= sizeof(strings));
    word(3); word((uint32_t)length); word((uint32_t)strings_len);
    memcpy(strings + strings_len, name, name_length); strings_len += name_length;
    CHECK(tree_len + length + 3 <= sizeof(tree));
    if (length) memcpy(tree + tree_len, bytes, length);
    tree_len += length;
    while (tree_len & 3) tree[tree_len++] = 0;
}
static void cell(const char* name, uint32_t value) { uint8_t bytes[4]; put32(bytes, value); prop(name, bytes, 4); }
static void root(uint32_t cells) {
    tree_len = strings_len = reservation_count = 0;
    begin(""); cell("#address-cells", cells); cell("#size-cells", cells);
}
static void reg(const char* name, uint64_t address, uint64_t size, uint32_t cells) {
    uint8_t bytes[16];
    if (cells == 2) { put64(bytes, address); put64(bytes + 8, size); }
    else { put32(bytes, (uint32_t)address); put32(bytes + 4, (uint32_t)size); }
    prop(name, bytes, cells * 8);
}
static void memory(uint64_t address, uint64_t size, uint32_t cells) {
    begin("memory@0"); prop("device_type", "memory", 7); reg("reg", address, size, cells); end();
}
static void reserved(uint32_t cells) {
    begin("reserved-memory"); cell("#address-cells", cells); cell("#size-cells", cells); prop("ranges", NULL, 0);
}
static size_t finish(void) {
    size_t structure, string_start, total, i;
    end(); word(9);
    structure = 40 + (reservation_count + 1) * 16;
    string_start = structure + tree_len;
    total = string_start + strings_len;
    CHECK(total <= sizeof(blob));
    memset(blob, 0, total);
    put32(blob, 0xd00dfeed); put32(blob + 4, (uint32_t)total);
    put32(blob + 8, (uint32_t)structure); put32(blob + 12, (uint32_t)string_start);
    put32(blob + 16, 40); put32(blob + 20, 17); put32(blob + 24, 16);
    put32(blob + 32, (uint32_t)strings_len); put32(blob + 36, (uint32_t)tree_len);
    for (i = 0; i < reservation_count; i++) {
        put64(blob + 40 + i * 16, reservations[i].start);
        put64(blob + 48 + i * 16, reservations[i].end); /* Builder stores size here. */
    }
    memcpy(blob + structure, tree, tree_len);
    memcpy(blob + string_start, strings, strings_len);
    return total;
}
static int discover(size_t length) {
    mm_discovery_init(&info);
    available.entries = available_storage; available.count = 0; available.capacity = MM_MAX_RANGES;
    return mm_discover(blob, length, &info);
}
static int contains(const MmRanges* ranges, uint64_t start, uint64_t end_address) {
    MmRange range = {start, end_address}; return mm_ranges_contains(ranges, range);
}

static void discovery_tests(void) {
    size_t size;
    uint8_t address[8];
    root(2);
    memory(0, 0x40000000, 2); memory(0x80000000, 0x100000000ULL, 2);
    reservations[reservation_count++] = (MmRange){0x12345, 12};
    begin("chosen"); put64(address, 0x3000000); prop("linux,initrd-start", address, 8);
    put64(address, 0x3100000); prop("linux,initrd-end", address, 8); end();
    reserved(2); begin("firmware"); reg("reg", 0x1000000, 0x1000, 2); prop("no-map", NULL, 0); end(); end();
    size = finish(); CHECK(discover(size));
    CHECK(info.ram.count == 2 && contains(&info.ram, 0x80000000, MM_RAM_LIMIT));
    CHECK(contains(&info.reserved, 0x12345, 0x12351));
    CHECK(contains(&info.reserved, 0x3000000, 0x3100000));
    CHECK(mm_discovery_finish(&info, &available));
    CHECK(!contains(&available, 0x12000, 0x13000));
    CHECK(!contains(&available, 0x1000000, 0x1200000));
    CHECK(contains(&available, 0x1200000, 0x1201000));

    root(1); memory(0, 0x10000000, 1);
    begin("memory@20000000"); prop("device_type", "memory", 7); prop("status", "disabled", 9);
    reg("reg", 0x20000000, 0x10000000, 1); end();
    reserved(1); begin("pool"); cell("size", 0x100000); cell("alignment", 0x200000);
    reg("alloc-ranges", 0x2000000, 0x1000000, 1); prop("reusable", NULL, 0); end(); end();
    CHECK(discover(finish()));
    CHECK(mm_ranges_insert(&info.reserved, (MmRange){0, 0x2100000}));
    CHECK(mm_discovery_finish(&info, &available));
    CHECK(info.ram.count == 1 && contains(&info.reserved, 0x2200000, 0x2300000));
    CHECK(!contains(&available, 0x2200000, 0x2300000));

    root(1); memory(0, 0x10000000, 1); reserved(1); begin("pool"); cell("size", 1);
    prop("no-map", NULL, 0); reg("alloc-ranges", 0x100001, 0x1000000, 1); end(); end();
    CHECK(discover(finish())); CHECK(mm_discovery_finish(&info, &available));
    CHECK(info.no_map.count == 1 && contains(&info.no_map, 0x200000, 0x400000));
    /* Finishing again recomputes availability without allocating a second pool. */
    available.count = 0; CHECK(mm_discovery_finish(&info, &available));
    CHECK(!contains(&available, 0x200000, 0x400000));
}

static void malformed_tests(void) {
    size_t size, i, j;
    static const size_t offsets[] = {0, 4, 8, 12, 16, 24, 32, 36};
    static const uint8_t masks[] = {1, 0x20, 0x80, 0xff};
    root(2); memory(0, 0x40000000, 2); size = finish(); memcpy(saved, blob, size);
    for (i = 0; i < size; i++) CHECK(!discover(i));
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        memcpy(blob, saved, size); memset(blob + offsets[i], 0xff, 4); CHECK(!discover(size));
    }
    memcpy(blob, saved, size); put32(blob + 20, 18); CHECK(discover(size));
    put32(blob + 20, 16); CHECK(!discover(size));
    for (i = 0; i < size; i++) for (j = 0; j < sizeof(masks); j++) {
        memcpy(blob, saved, size); blob[i] ^= masks[j]; (void)discover(size);
    }
    for (i = 0; i < 8; i++) {
        root(1); memory(0, 0x10000000, 1); reserved(1); begin("bad");
        if (i == 0) { cell("size", 100); cell("alignment", 3); }
        if (i == 1) { reg("reg", 0x200000, 0x1000, 1); prop("no-map", NULL, 0); prop("reusable", NULL, 0); }
        if (i == 2) prop("reg", "x", 1);
        if (i == 3) { reg("reg", 0x200000, 0x1000, 1); prop("status", "okay", 4); }
        if (i == 4) { reg("reg", 0x200000, 0x1000, 1); reg("reg", 0x400000, 0x1000, 1); }
        if (i == 5) { cell("size", 1); reg("alloc-ranges", 0x100000, 0, 1); }
        if (i == 6) { cell("size", 0); }
        /* i == 7 intentionally has neither reg nor size. */
        end(); end(); CHECK(!discover(finish()));
    }
    root(1); memory(0, 0x10000000, 1);
    begin("reserved-memory"); cell("#address-cells", 1); cell("#size-cells", 1); cell("ranges", 0);
    begin("firmware"); reg("reg", 0x200000, 4096, 1); end(); end(); CHECK(!discover(finish()));
    root(1); memory(0, 0x1000000, 1);
    for (i = 0; i < 33; i++) begin("nested");
    for (i = 0; i < 33; i++) end();
    CHECK(!discover(finish()));
    root(1); memory(0, 0x1000000, 1); reserved(1); begin("pool"); cell("size", 0x2000000); end(); end();
    CHECK(discover(finish())); CHECK(!mm_discovery_finish(&info, &available));
    root(1); memory(0, 0x1000000, 1); reserved(1);
    for (i = 0; i < 17; i++) { char name[32]; snprintf(name, sizeof(name), "pool%u", (unsigned)i); begin(name); cell("size", 4096); end(); }
    end(); CHECK(!discover(finish()));
    root(1); memory(0, 0x10000000, 1);
    for (i = 0; i < 129; i++) reservations[reservation_count++] = (MmRange){0x100000 + i * 8192, 1};
    CHECK(!discover(finish()));
    root(1); prop("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", NULL, 0); memory(0, 0x10000000, 1); CHECK(!discover(finish()));
    root(1); memory(0, 0x10000000, 1); reservations[reservation_count++] = (MmRange){~(uint64_t)0 - 10, 20};
    CHECK(!discover(finish()));
    root(1); memory(0, 0x10000000, 1); begin("chosen"); cell("linux,initrd-start", 0x200000); end();
    CHECK(!discover(finish()));
    root(1); memory(0, 0x10000000, 1); begin("chosen"); reg("linux,usable-memory-range", 0, 0x2000000, 1); end();
    CHECK(!discover(finish()));
}

int main(void) {
    discovery_tests();
    malformed_tests();
    puts("PASS: DTB RAM/reservations/dynamic pools, malformed inputs and deterministic mutations");
    return 0;
}
