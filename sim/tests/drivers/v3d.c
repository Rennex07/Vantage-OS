#include "../../../includes/video/v3d.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define HUB 0xfec00000ULL
#define CORE 0xfec04000ULL
static unsigned char table_storage[V3D_TABLE_BYTES + 65535U];
static unsigned char* table;
static HW_ALIGN(4096) unsigned char arena[V3D_ARENA_BYTES];
typedef struct { V3d* gpu; uint32_t clock, bin, render, end, start; int owner, mode, submissions, clean, invalidated, stopped; } Model;
static uint32_t get32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static unsigned char* gpu_address(uint32_t address) {
    CHECK(address >= V3D_GPU_BASE && address < V3D_GPU_BASE + sizeof(arena));
    return arena + address - V3D_GPU_BASE;
}
static void render(Model* m) {
    unsigned char* list = gpu_address(m->start);
    unsigned int size = m->end - m->start, offset = 0, tiles = 0;
    uint32_t color = 0, generic = 0;
    CHECK(size < 4096 && list[0] == 121 && list[1] == 0);
    while (offset < size) {
        unsigned char* p = list + offset;
        unsigned int count = 0;
        switch (*p) {
        case 121: count = 9; if (p[1] == 3) color = get32(p + 2); break;
        case 126: case 25: count = 2; break;
        case 123: count = 5; CHECK(get32(p + 1) == V3D_GPU_BASE + 0x10000); break;
        case 122: count = 9; break;
        case 124: count = 4; break;
        case 26: case 27: case 19: case 13: count = 1; break;
        case 29: count = 13; CHECK(p[1] == 8); break;
        case 20: count = 9; generic = get32(p + 1); CHECK(get32(p + 5) > generic); break;
        case 23:
            count = 3; CHECK(p[1] < (m->gpu->width + 63) / 64 && p[2] < (m->gpu->height + 63) / 64); tiles++; break;
        default: CHECK(0);
        }
        CHECK(count && count <= size - offset); offset += count;
    }
    CHECK(list[size - 1] == 13 && tiles == ((m->gpu->width + 63) / 64) * ((m->gpu->height + 63) / 64));
    unsigned char* indirect = gpu_address(generic); CHECK(*indirect++ == 125);
    unsigned char* source = NULL;
    if (*indirect == 30) {
        CHECK(((unsigned int)indirect[2] | ((unsigned int)indirect[3] << 8)) == (27U << 4));
        source = gpu_address(get32(indirect + 9)); indirect += 13;
    }
    CHECK(*indirect++ == 26 && *indirect == 29);
    CHECK((get32(indirect + 4) >> 4) == m->gpu->pitch);
    unsigned char* destination = gpu_address(get32(indirect + 9));
    CHECK(destination == arena + 0x40000 || destination == arena + 0x200000);
    if (source) memcpy(destination, source, m->gpu->bytes);
    else for (unsigned int i = 0; i < m->gpu->bytes; i += 4) memcpy(destination + i, &color, 4);
    CHECK(indirect[13] == 25 && indirect[15] == 27 && indirect[16] == 18);
}
static uint32_t rd(void* context, uintptr_t address) {
    Model* m = context;
    if (address == CORE) return m->mode == 3 ? 0x03443356U : 0x04443356U;
    if (address == CORE + 4) return 2;
    if (address == HUB + 0xc) return 0x100;
    if (address == HUB + 0x10) return 0x100;
    if (address == CORE + 0x134) return m->bin;
    if (address == CORE + 0x138) return m->render;
    if (address == HUB + 0x50 && m->mode == 2 && m->submissions) return 0x10;
    return 0;
}
static void wr(void* context, uintptr_t address, uint32_t value) {
    Model* m = context;
    if (address == CORE + 0x168) {
        CHECK(value > V3D_GPU_BASE && value < V3D_GPU_BASE + 4096);
        CHECK(arena[0] == 120 && arena[value - V3D_GPU_BASE - 1] == 4);
        m->submissions++;
        if (m->mode != 1) m->bin = (m->bin + 1) & 255U;
    }
    if (address == CORE + 0x164) m->start = value;
    if (address == CORE + 0x16c) { m->end = value; render(m); m->render = (m->render + 1) & 255U; }
    if (address == CORE + 0x804 && value == 2) m->stopped++;
}
static uint32_t clock_us(void* context) { Model* m = context; m->clock += 1000; return m->clock; }
static int owned(void* context) { return ((Model*)context)->owner; }
static int property(void* context, uint32_t tag, uint32_t* words, unsigned int count) {
    (void)context; CHECK(count == 2);
    if (tag == 0x30002) { CHECK(words[0] == 5); words[1] = 500000000; }
    else CHECK(tag == 0x38030 && words[0] == 11 && words[1] == 1);
    return HW_OK;
}
static uint64_t physical(void* context, const void* memory) {
    (void)context; uintptr_t p = (uintptr_t)memory;
    if (p >= (uintptr_t)table && p < (uintptr_t)table + V3D_TABLE_BYTES) return 0x1000000U + p - (uintptr_t)table;
    CHECK(p >= (uintptr_t)arena && p < (uintptr_t)arena + sizeof(arena));
    return 0x2000000U + p - (uintptr_t)arena;
}
static void cache(void* context, void* memory, size_t bytes, enum HwCache operation) {
    Model* m = context; CHECK(memory && bytes);
    if (operation == HW_INVALIDATE) m->invalidated++;
    else m->clean++;
}
int main(void) {
    table = (unsigned char*)(((uintptr_t)table_storage + 65535U) & ~(uintptr_t)65535U);
    V3d gpu = {0}; Model m = {0}; m.gpu = &gpu; m.owner = 1; m.bin = m.render = 255;
    HwOps io = {&m, rd, wr, clock_us, owned, cache, physical, property};
    CHECK(v3d_init(&gpu, &io, table, arena, 800, 480) == HW_OK);
    const uint32_t* pte = (const uint32_t*)table;
    for (unsigned int page = 0; page < V3D_TABLE_BYTES / 4; page++) {
        if (page >= (V3D_GPU_BASE >> 12) && page < (V3D_GPU_BASE >> 12) + V3D_ARENA_BYTES / 4096)
            CHECK(pte[page] == (0x30000000U | (0x2000000U >> 12) | (page - (V3D_GPU_BASE >> 12))));
        else CHECK(!pte[page]);
    }
    CHECK(v3d_self_test(&gpu) == 1 && m.submissions == 3 && m.invalidated == 3);
    CHECK(v3d_clear(&gpu, 2, 0) == HW_INVALID && v3d_copy(&gpu, 0, 2) == HW_INVALID);
    m.owner = 0; CHECK(v3d_clear(&gpu, 0, 0) == HW_INVALID); m.owner = 1;
    m.mode = 1; m.clock = 0xffff0000U;
    CHECK(v3d_clear(&gpu, 0, 0) == HW_TIMEOUT && gpu.state == -1 && m.stopped);
    CHECK(v3d_pixels(&gpu, 0) == NULL && v3d_clear(&gpu, 0, 0) == HW_OFF);
    memset(&gpu, 0, sizeof(gpu)); m.mode = 3;
    CHECK(v3d_init(&gpu, &io, table, arena, 64, 64) == HW_UNSUPPORTED);
    memset(&gpu, 0, sizeof(gpu)); m.mode = 0; m.submissions = 0;
    CHECK(v3d_init(&gpu, &io, table, arena, 64, 64) == HW_OK);
    m.mode = 2;
    CHECK(v3d_clear(&gpu, 0, 0) == HW_IO && gpu.state == -1);
    puts("PASS: V3D packet decoder/model, private PTEs, counter wrap, ownership and timeout quarantine");
    return 0;
}
