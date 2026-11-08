/* Portions of MMU/cache sequencing Copyright (C) 2017-2018 Broadcom,
 * used under GPL version 3 through upstream's GPL-2.0-or-later grant.
 * V3D 4.2 packet layout follows Random06457's MIT-licensed Pi 4 examples.
 * MMU/cache register semantics: Linux v6.12 drivers/gpu/drm/v3d (GPL-2.0+).
 * See docs/licenses/v3d-examples-MIT.txt and docs/HARDWARE.md. */
#include "../../includes/video/v3d.h"

#define HUB 0xfec00000ULL
#define CORE 0xfec04000ULL
#define SURFACE0 0x40000U
#define SURFACE1 0x200000U
#define TILE_ALLOC 0x10000U
#define TILE_STATE 0x20000U
#define RCL 0x1000U
#define INDIRECT 0x2000U
#define MMU_CONTROL 0x01200U
#define MMU_ENABLE ((1U << 26) | (1U << 25) | (1U << 19) | (1U << 18) | \
                    (1U << 16) | (1U << 11) | (1U << 10) | 1U)

static uint32_t rd(V3d* gpu, uintptr_t address) { return gpu->io->read(gpu->io->context, address); }
static void wr(V3d* gpu, uintptr_t address, uint32_t value) { gpu->io->write(gpu->io->context, address, value); }
static void cache(V3d* gpu, void* memory, size_t bytes, enum HwCache operation) {
    gpu->io->cache(gpu->io->context, memory, bytes, operation);
}

static int fail(V3d* gpu, int error) {
    gpu->state = -1;
    gpu->error = error;
    /* Stop new AXI requests. No caller may reuse/free any of these pages even
     * if an in-flight transaction does not drain before this bounded wait. */
    wr(gpu, CORE + 0x804, 2);
    (void)hw_wait(gpu->io, CORE + 0x800, 0x7f7f0038U, 0, 100000);
    return error;
}

static void put16(unsigned char* p, uint32_t value) { p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8); }
static void put32(unsigned char* p, uint32_t value) {
    for (unsigned int i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (i * 8));
}

typedef struct { unsigned char* data; unsigned int size; int valid; } List;
static unsigned char* packet(List* list, unsigned int bytes, unsigned int opcode) {
    if (!list->valid || !bytes || list->size > 4096U - bytes) { list->valid = 0; return NULL; }
    unsigned char* p = list->data + list->size;
    memset(p, 0, bytes);
    p[0] = (unsigned char)opcode;
    list->size += bytes;
    return p;
}

static uint32_t surface_offset(unsigned int surface) { return surface ? SURFACE1 : SURFACE0; }

static void tile_transfer(List* list, int load, uint32_t gpu_address, uint32_t pitch) {
    unsigned char* p = packet(list, 13, load ? 30 : 29);
    if (!p) return;
    /* Render target 0, raster layout, RGBA8, no decimation/swizzling. */
    put16(p + 2, 27U << 4);
    put32(p + 4, pitch << 4);
    put32(p + 9, gpu_address);
}

static unsigned int build_lists(V3d* gpu, unsigned int destination, int source, uint32_t color,
                                unsigned int* bin_bytes) {
    List bin = {gpu->arena, 0, 1}, render = {gpu->arena + RCL, 0, 1};
    List indirect = {gpu->arena + INDIRECT, 0, 1};
    unsigned char* p = packet(&bin, 9, 120);
    put16(p + 5, gpu->width - 1); put16(p + 7, gpu->height - 1);
    p = packet(&bin, 2, 119); p[1] = 0; /* One layer; do not inherit firmware state. */
    packet(&bin, 1, 19); packet(&bin, 5, 92); packet(&bin, 1, 6); packet(&bin, 1, 4);

    p = packet(&render, 9, 121);
    put16(p + 2, gpu->width); put16(p + 4, gpu->height); put16(p + 6, 2U << 7);
    p = packet(&render, 9, 121); p[1] = 3; put32(p + 2, color);
    p = packet(&render, 9, 121); p[1] = 0x81; /* sub-ID 1, internal type 8-bit UNORM. */
    p = packet(&render, 9, 121); p[1] = 2; put32(p + 3, 0x3f800000U);
    p = packet(&render, 2, 126); p[1] = 4;
    p = packet(&render, 5, 123); put32(p + 1, V3D_GPU_BASE + TILE_ALLOC);
    uint32_t columns = (gpu->width + 63) / 64, rows = (gpu->height + 63) / 64;
    p = packet(&render, 9, 122);
    p[3] = (unsigned char)columns; p[4] = (unsigned char)rows;
    put32(p + 5, columns | (rows << 12));
    /* V3D 4.x initial tile clear and required dummy-store race workaround. */
    for (unsigned int i = 0; i < 2; i++) {
        packet(&render, 4, 124); packet(&render, 1, 26);
        p = packet(&render, 13, 29); p[1] = 8; /* Store no buffers. */
        if (!i) { p = packet(&render, 2, 25); p[1] = 3; }
        packet(&render, 1, 27);
    }
    packet(&render, 1, 19);
    packet(&indirect, 1, 125);
    if (source >= 0) tile_transfer(&indirect, 1, V3D_GPU_BASE + surface_offset((unsigned int)source), gpu->pitch);
    packet(&indirect, 1, 26);
    tile_transfer(&indirect, 0, V3D_GPU_BASE + surface_offset(destination), gpu->pitch);
    p = packet(&indirect, 2, 25); p[1] = 3;
    packet(&indirect, 1, 27); packet(&indirect, 1, 18);
    p = packet(&render, 9, 20);
    put32(p + 1, V3D_GPU_BASE + INDIRECT);
    put32(p + 5, V3D_GPU_BASE + INDIRECT + indirect.size);
    for (unsigned int y = 0; y < rows; y++) for (unsigned int x = 0; x < columns; x++) {
        p = packet(&render, 3, 23);
        if (p) { p[1] = (unsigned char)x; p[2] = (unsigned char)y; }
    }
    packet(&render, 1, 13);
    if (!bin.valid || !render.valid || !indirect.valid) return 0;
    cache(gpu, gpu->arena, 3 * 4096, HW_CLEAN);
    *bin_bytes = bin.size;
    return render.size;
}

static int errors(V3d* gpu) {
    return (rd(gpu, HUB + 0x50) & 0x38U) ||
           (rd(gpu, CORE + 0x50) & 0x24U) || rd(gpu, CORE + 0xf20);
}

static int cache_gpu(V3d* gpu, int clean) {
    wr(gpu, CORE + 0x34, 0); wr(gpu, CORE + 0x38, ~0U);
    if (clean) {
        wr(gpu, CORE + 0x30, 1U << 8);
        if (hw_wait(gpu->io, CORE + 0x30, 1U << 8, 0, 100000) != HW_OK) return HW_TIMEOUT;
    }
    wr(gpu, CORE + 0x30, clean ? 5U : 1U);
    if (hw_wait(gpu->io, CORE + 0x30, 1, 0, 100000) != HW_OK) return HW_TIMEOUT;
    if (!clean) wr(gpu, CORE + 0x24, ~0U);
    return HW_OK;
}

static int complete(V3d* gpu, uintptr_t counter, uint32_t previous) {
    uint32_t start = gpu->io->time_us(gpu->io->context);
    for (unsigned int i = 0; i < 8000000U; i++) {
        if (errors(gpu)) return HW_IO;
        if ((rd(gpu, counter) & 255U) != previous) return HW_OK;
        if (gpu->io->time_us(gpu->io->context) - start >= 500000U) break;
    }
    return HW_TIMEOUT;
}

int v3d_init(V3d* gpu, const HwOps* io, void* table, void* arena,
             uint32_t width, uint32_t height) {
    if (!gpu || !hw_ops_valid(io) || !io->cache || !io->physical || !io->property ||
        !io->owned(io->context) || !table || !arena || !width || !height ||
        width > 800 || height > 800 || (width & 3) || (uint64_t)width * height > 384000 ||
        ((uintptr_t)table & 65535U) || ((uintptr_t)arena & 4095U)) return HW_INVALID;
    if (gpu->state) return HW_BUSY;
    uint64_t pt = io->physical(io->context, table), memory = io->physical(io->context, arena);
    if (!pt || (pt & 65535U) || !memory || (memory & 4095U) ||
        pt > 0xfc000000ULL - V3D_TABLE_BYTES || memory > 0xfc000000ULL - V3D_ARENA_BYTES ||
        (pt < memory + V3D_ARENA_BYTES && memory < pt + V3D_TABLE_BYTES) ||
        io->physical(io->context, (unsigned char*)table + V3D_TABLE_BYTES - 1) != pt + V3D_TABLE_BYTES - 1 ||
        io->physical(io->context, (unsigned char*)arena + V3D_ARENA_BYTES - 1) != memory + V3D_ARENA_BYTES - 1)
        return HW_INVALID;
    gpu->io = io; gpu->table = table; gpu->arena = arena;
    gpu->width = width; gpu->height = height; gpu->pitch = width * 4; gpu->bytes = width * height * 4;
    gpu->state = -1;
    uint32_t domain[2] = {11, 1};
    if (io->property(io->context, 0x00038030, domain, 2) != HW_OK || domain[0] != 11 || domain[1] != 1)
        return HW_IO;
    uint32_t clock[2] = {5, 0};
    if (io->property(io->context, 0x00030002, clock, 2) != HW_OK || clock[0] != 5 || !clock[1]) return HW_IO;
    wr(gpu, 0xfe10010c, 0x5a000000U | rd(gpu, 0xfe10010c) | 64U);
    for (unsigned int i = 0; i < 2; i++) {
        uintptr_t bridge = 0xfec1100cULL - i * 4;
        wr(gpu, bridge, 0x5a000000U | (rd(gpu, bridge) & ~1U));
        if (hw_wait(io, bridge, 2, 0, 100000) != HW_OK) return HW_TIMEOUT;
    }
    /* Only V3D 4.2, one core, and a present MMU are supported. */
    uint32_t ident = rd(gpu, HUB + 0xc);
    if ((rd(gpu, CORE) & 0xffffffU) != 0x00443356U || ((rd(gpu, CORE) >> 24) & 255U) != 4 ||
        (rd(gpu, CORE + 4) & 15U) != 2 || ((ident >> 8) & 15U) != 1 || !(rd(gpu, HUB + 0x10) & 256U))
        return HW_UNSUPPORTED;
    if ((rd(gpu, CORE + 0x100) | rd(gpu, CORE + 0x104)) & 0x20U ||
        (rd(gpu, CORE + 0x900) & 15U) || (rd(gpu, HUB + 0x400) & 1U)) return HW_BUSY;
    memset(table, 0, V3D_TABLE_BYTES); memset(arena, 0, V3D_ARENA_BYTES);
    for (unsigned int page = 0; page < V3D_ARENA_BYTES / 4096; page++)
        gpu->table[(V3D_GPU_BASE >> 12) + page] = (uint32_t)((memory >> 12) + page) | 0x30000000U;
    cache(gpu, table, V3D_TABLE_BYTES, HW_CLEAN);
    cache(gpu, arena, V3D_ARENA_BYTES, HW_CLEAN_INVALIDATE);
    wr(gpu, CORE + 0x60, ~0U); wr(gpu, HUB + 0x60, ~0U);
    wr(gpu, CORE + 0x58, ~0U); wr(gpu, HUB + 0x58, ~0U);
    wr(gpu, CORE + 0x804, 0);
    wr(gpu, HUB + 0x121c, 0); wr(gpu, HUB + 0x1220, 0); /* Disable bypass range. */
    wr(gpu, HUB + 0x1230, 0); /* Invalid accesses abort; no scratch remapping. */
    wr(gpu, HUB + 0x1204, (uint32_t)(pt >> 12));
    wr(gpu, HUB + MMU_CONTROL, MMU_ENABLE);
    if (hw_wait(io, HUB + MMU_CONTROL, 128, 0, 100000) != HW_OK) return fail(gpu, HW_TIMEOUT);
    wr(gpu, HUB + MMU_CONTROL, MMU_ENABLE | 4U); wr(gpu, HUB + 0x1000, 3);
    if (hw_wait(io, HUB + MMU_CONTROL, 128, 0, 100000) != HW_OK ||
        hw_wait(io, HUB + 0x1000, 4, 0, 100000) != HW_OK) return fail(gpu, HW_TIMEOUT);
    gpu->error = 0; gpu->state = 1;
    return HW_OK;
}

static int submit(V3d* gpu, unsigned int destination, int source, uint32_t color) {
    if (!gpu || !gpu->io || !gpu->io->owned(gpu->io->context) || destination > 1 || source > 1) return HW_INVALID;
    if (gpu->state != 1) return gpu->state == 2 ? HW_BUSY : HW_OFF;
    if (source == (int)destination) return HW_OK;
    gpu->state = 2;
    if (source >= 0) cache(gpu, gpu->arena + surface_offset((unsigned int)source), gpu->bytes, HW_CLEAN);
    cache(gpu, gpu->arena + surface_offset(destination), gpu->bytes, HW_CLEAN_INVALIDATE);
    unsigned int bin_bytes = 0, render_bytes = build_lists(gpu, destination, source, color, &bin_bytes);
    if (!render_bytes || cache_gpu(gpu, 0) != HW_OK) return fail(gpu, HW_PROTOCOL);
    uint32_t count = rd(gpu, CORE + 0x134) & 255U;
    wr(gpu, CORE + 0x170, V3D_GPU_BASE + TILE_ALLOC); wr(gpu, CORE + 0x174, 65536);
    wr(gpu, CORE + 0x15c, V3D_GPU_BASE + TILE_STATE + 2);
    wr(gpu, CORE + 0x160, V3D_GPU_BASE); wr(gpu, CORE + 0x168, V3D_GPU_BASE + bin_bytes);
    int result = complete(gpu, CORE + 0x134, count);
    if (result != HW_OK) return fail(gpu, result);
    if (cache_gpu(gpu, 0) != HW_OK) return fail(gpu, HW_TIMEOUT);
    count = rd(gpu, CORE + 0x138) & 255U;
    wr(gpu, CORE + 0x178, 0);
    wr(gpu, CORE + 0x164, V3D_GPU_BASE + RCL); wr(gpu, CORE + 0x16c, V3D_GPU_BASE + RCL + render_bytes);
    result = complete(gpu, CORE + 0x138, count);
    if (result != HW_OK) return fail(gpu, result);
    if (cache_gpu(gpu, 1) != HW_OK) return fail(gpu, HW_TIMEOUT);
    cache(gpu, gpu->arena + surface_offset(destination), gpu->bytes, HW_INVALIDATE);
    gpu->state = 1;
    return HW_OK;
}

int v3d_clear(V3d* gpu, unsigned int surface, uint32_t rgba) { return submit(gpu, surface, -1, rgba); }
int v3d_copy(V3d* gpu, unsigned int destination, unsigned int source) {
    if (source > 1) return HW_INVALID;
    return submit(gpu, destination, (int)source, 0);
}
uint32_t* v3d_pixels(V3d* gpu, unsigned int surface) {
    if (!gpu || !gpu->io || gpu->state != 1 || surface > 1 || !gpu->io->owned(gpu->io->context)) return NULL;
    return (uint32_t*)(gpu->arena + surface_offset(surface));
}
int v3d_self_test(V3d* gpu) {
    if (v3d_clear(gpu, 0, 0xff332211U) != HW_OK || v3d_copy(gpu, 1, 0) != HW_OK) return 0;
    uint32_t* first = v3d_pixels(gpu, 0), *second = v3d_pixels(gpu, 1);
    if (!first || !second) return 0;
    for (unsigned int i = 0; i < gpu->width * gpu->height; i++)
        if (first[i] != 0xff332211U || second[i] != first[i]) return fail(gpu, HW_IO), 0;
    return v3d_clear(gpu, 0, 0) == HW_OK;
}
