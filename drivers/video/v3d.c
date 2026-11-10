/* Portions of MMU/cache/reset sequencing Copyright (C) 2017-2018 Broadcom,
 * used under GPL version 3 through upstream's GPL-2.0-or-later grant.
 * V3D 4.2 packet layout follows Random06457's MIT-licensed Pi 4 examples and
 * Mesa's MIT-licensed src/broadcom/cle/v3d_packet.xml.
 * Register, MMU, cache and power-bridge semantics: Linux v6.12
 * drivers/gpu/drm/v3d and drivers/pmdomain/bcm/bcm2835-power.c (GPL-2.0+).
 * See docs/licenses/v3d-examples-MIT.txt and docs/HARDWARE.md. */
#include <vantage/drivers/v3d.h>
#include <vantage/kernel/render.h>

#define HUB 0xfec00000ULL
#define CORE 0xfec04000ULL

/* Hub registers. */
#define HUB_IDENT1 (HUB + 0x0c)
#define HUB_IDENT2 (HUB + 0x10)
#define HUB_IDENT2_WITH_MMU (1U << 8)
#define HUB_INT_STS (HUB + 0x50)
#define HUB_INT_CLR (HUB + 0x58)
#define HUB_INT_MSK_SET (HUB + 0x60)
#define HUB_INT_MMU (0x38U) /* Write violation, invalid PTE, address cap. */
#define TFU_CS (HUB + 0x400)
#define MMUC_CONTROL (HUB + 0x1000)
#define MMUC_FLUSHING (1U << 2)
#define MMUC_FLUSH (1U << 1)
#define MMUC_ENABLE (1U << 0)
#define MMU_CTL (HUB + 0x1200)
#define MMU_CTL_FAULTS ((1U << 27) | (1U << 20) | (1U << 12))
#define MMU_CTL_TLB_CLEARING (1U << 7)
#define MMU_CTL_TLB_CLEAR (1U << 2)
/* Enable; abort and flag invalid PTEs, write violations and cap overruns. */
#define MMU_CTL_CONFIG ((1U << 26) | (1U << 25) | (1U << 19) | (1U << 18) | \
                        (1U << 16) | (1U << 11) | (1U << 10) | 1U)
#define MMU_PT_PA_BASE (HUB + 0x1204)
#define MMU_ILLEGAL_ADDR (HUB + 0x1230)

/* Core 0 registers. */
#define CTL_IDENT0 (CORE + 0x00)
#define CTL_IDENT1 (CORE + 0x04)
#define CTL_SLCACTL (CORE + 0x24)
#define CTL_L2TCACTL (CORE + 0x30)
#define CTL_L2TFLSTA (CORE + 0x34)
#define CTL_L2TFLEND (CORE + 0x38)
#define CTL_INT_STS (CORE + 0x50)
#define CTL_INT_CLR (CORE + 0x58)
#define CTL_INT_MSK_SET (CORE + 0x60)
#define CTL_INT_FAULTS (0x24U) /* Binner out of memory, GMP violation. */
#define CLE_BFC (CORE + 0x134)
#define CLE_RFC (CORE + 0x138)
#define CLE_CT0QTS (CORE + 0x15c)
#define CLE_CT0QBA (CORE + 0x160)
#define CLE_CT1QBA (CORE + 0x164)
#define CLE_CT0QEA (CORE + 0x168)
#define CLE_CT1QEA (CORE + 0x16c)
#define CLE_CT0QMA (CORE + 0x170)
#define CLE_CT0QMS (CORE + 0x174)
#define GMP_STATUS (CORE + 0x800)
#define GMP_STATUS_BUSY (0x7f7f0008U) /* Outstanding reads/writes, config busy. */
#define GMP_CFG (CORE + 0x804)
#define GMP_CFG_STOP_REQ (1U << 1)
#define CSD_STATUS (CORE + 0x900)
#define ERR_STAT (CORE + 0xf20)

/* BCM2711 power management block and the V3D AXI bridge. */
#define PM_GRAFX 0xfe10010cULL
#define PM_V3DRSTN (1U << 6)
#define PM_PASSWORD 0x5a000000U
#define ASB_V3D_S_CTRL 0xfec11008ULL
#define ASB_V3D_M_CTRL 0xfec1100cULL
#define ASB_REQ_STOP (1U << 0)
#define ASB_ACK (1U << 1)

/* Firmware property tags and identifiers. */
#define TAG_SET_CLOCK_STATE 0x00038001U
#define TAG_GET_CLOCK_RATE 0x00030002U
#define TAG_SET_DOMAIN_STATE 0x00038030U
#define FIRMWARE_DOMAIN_V3D 11U
#define FIRMWARE_CLOCK_V3D 5U

#define PTE_VALID_WRITEABLE 0x30000000U
#define RAM_LIMIT 0xfc000000ULL
#define WAIT_US 100000U
#define JOB_US 500000U
#define JOB_POLLS 8000000U

/* Arena layout. Everything up to the guard page is mapped at V3D_GPU_BASE;
 * the last arena page is the MMU scratch page and is never mapped. */
#define BIN_LIST 0x0000U
#define RENDER_LIST 0x1000U
#define TILE_LIST 0x2000U
#define LIST_BYTES 0x1000U
#define TILE_ALLOC 0x10000U
#define TILE_ALLOC_BYTES 0x10000U
#define TILE_STATE 0x20000U
#define SURFACE 0x40000U
#define SCRATCH (V3D_ARENA_BYTES - 4096U)
#define GUARD_WORD 0x6b6e6147U

enum { LEVEL_NONE, LEVEL_BRIDGE, LEVEL_CORE };

static uint32_t rd(V3d* gpu, uintptr_t address) { return gpu->io->read(gpu->io->context, address); }
static void wr(V3d* gpu, uintptr_t address, uint32_t value) { gpu->io->write(gpu->io->context, address, value); }
static int settle(V3d* gpu, uintptr_t address, uint32_t mask, uint32_t value) {
    return hw_wait(gpu->io, address, mask, value, WAIT_US);
}
static void cache(V3d* gpu, void* memory, size_t bytes, enum HwCache operation) {
    gpu->io->cache(gpu->io->context, memory, bytes, operation);
}

static int bridge(V3d* gpu, uintptr_t control, int run) {
    uint32_t value = rd(gpu, control);
    wr(gpu, control, PM_PASSWORD | (run ? value & ~ASB_REQ_STOP : value | ASB_REQ_STOP));
    return settle(gpu, control, ASB_ACK, run ? 0 : ASB_ACK);
}

/* Returns nonzero only when both bridges acknowledged the stop and the reset
 * line reads back asserted: the core can then no longer reach memory. */
static int hold_reset(V3d* gpu) {
    int slave = bridge(gpu, ASB_V3D_S_CTRL, 0), master = bridge(gpu, ASB_V3D_M_CTRL, 0);
    wr(gpu, PM_GRAFX, PM_PASSWORD | (rd(gpu, PM_GRAFX) & ~PM_V3DRSTN));
    return slave == HW_OK && master == HW_OK && !(rd(gpu, PM_GRAFX) & PM_V3DRSTN);
}

static void shutdown(V3d* gpu) {
    int confirmed = 1;
    if (gpu->level >= LEVEL_CORE) {
        gpu->detail = rd(gpu, ERR_STAT);
        wr(gpu, CTL_INT_MSK_SET, ~0U); wr(gpu, HUB_INT_MSK_SET, ~0U);
        /* Let outstanding AXI transactions drain before the bridges stop. */
        wr(gpu, GMP_CFG, GMP_CFG_STOP_REQ);
        (void)settle(gpu, GMP_STATUS, GMP_STATUS_BUSY, 0);
    }
    if (gpu->level >= LEVEL_BRIDGE) confirmed = hold_reset(gpu);
    uint32_t domain[2] = {FIRMWARE_DOMAIN_V3D, 0};
    (void)gpu->io->property(gpu->io->context, TAG_SET_DOMAIN_STATE, domain, 2);
    gpu->present = 0;
    gpu->state = !gpu->mapped || confirmed ? V3D_STOPPED : V3D_QUARANTINED;
}

static int fail(V3d* gpu, int stage, int error) {
    gpu->stage = stage;
    gpu->error = error;
    shutdown(gpu);
    return error;
}

static int mmu_flush(V3d* gpu) {
    if (settle(gpu, MMU_CTL, MMU_CTL_TLB_CLEARING, 0) != HW_OK) return HW_TIMEOUT;
    wr(gpu, MMU_CTL, rd(gpu, MMU_CTL) | MMU_CTL_TLB_CLEAR);
    wr(gpu, MMUC_CONTROL, MMUC_FLUSH | MMUC_ENABLE);
    if (settle(gpu, MMU_CTL, MMU_CTL_TLB_CLEARING, 0) != HW_OK ||
        settle(gpu, MMUC_CONTROL, MMUC_FLUSHING, 0) != HW_OK) return HW_TIMEOUT;
    return HW_OK;
}

/* Page-aligned physical run of size bytes below the peripheral window that
 * does not overlap the driver's own buffers; zero otherwise. */
static uint64_t physical_run(V3d* gpu, const HwOps* io, const void* memory, uint64_t alignment, uint64_t size) {
    uint64_t start = io->physical(io->context, memory);
    if (!start || (start & (alignment - 1)) || start > RAM_LIMIT - size ||
        io->physical(io->context, (const unsigned char*)memory + size - 1) != start + size - 1) return 0;
    if (gpu) {
        uint64_t table = io->physical(io->context, gpu->table), arena = io->physical(io->context, gpu->arena);
        if ((start < table + V3D_TABLE_BYTES && table < start + size) ||
            (start < arena + V3D_ARENA_BYTES && arena < start + size)) return 0;
    }
    return start;
}

/* Map (address != 0) or unmap the present window, then flush the device TLB. */
static int map_target(V3d* gpu, uint64_t address) {
    uint32_t* entry = gpu->table + (V3D_TARGET_BASE >> 12);
    uint32_t pages = gpu->bytes >> 12;
    for (uint32_t page = 0; page < pages; page++)
        entry[page] = address ? (uint32_t)((address >> 12) + page) | PTE_VALID_WRITEABLE : 0;
    cache(gpu, entry, pages * sizeof(uint32_t), HW_CLEAN);
    return mmu_flush(gpu);
}

static void put16(unsigned char* p, uint32_t value) { p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8); }
static void put32(unsigned char* p, uint32_t value) {
    for (unsigned int i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (i * 8));
}

typedef struct { unsigned char* data; unsigned int size; int valid; } List;
static unsigned char* packet(List* list, unsigned int bytes, unsigned int opcode) {
    if (!list->valid || !bytes || list->size > LIST_BYTES - bytes) { list->valid = 0; return NULL; }
    unsigned char* p = list->data + list->size;
    memset(p, 0, bytes);
    p[0] = (unsigned char)opcode;
    list->size += bytes;
    return p;
}

static void tile_transfer(List* list, int load, uint32_t gpu_address, uint32_t pitch) {
    unsigned char* p = packet(list, 13, load ? 30 : 29);
    if (!p) return;
    /* Render target 0, raster layout, RGBA8, no decimation/swizzling. */
    put16(p + 2, 27U << 4);
    put32(p + 4, pitch << 4);
    put32(p + 9, gpu_address);
}

/* Every tile either stores the clear colour or loads the surface, then
 * stores to store_address. Returns the render list size, zero on overflow. */
static unsigned int build_lists(V3d* gpu, int load, uint32_t store_address, uint32_t color,
                                unsigned int* bin_bytes) {
    List bin = {gpu->arena + BIN_LIST, 0, 1}, render = {gpu->arena + RENDER_LIST, 0, 1};
    List tile = {gpu->arena + TILE_LIST, 0, 1};
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
    packet(&tile, 1, 125);
    if (load) tile_transfer(&tile, 1, V3D_GPU_BASE + SURFACE, gpu->pitch);
    packet(&tile, 1, 26);
    tile_transfer(&tile, 0, store_address, gpu->pitch);
    p = packet(&tile, 2, 25); p[1] = 3;
    packet(&tile, 1, 27); packet(&tile, 1, 18);
    p = packet(&render, 9, 20);
    put32(p + 1, V3D_GPU_BASE + TILE_LIST);
    put32(p + 5, V3D_GPU_BASE + TILE_LIST + tile.size);
    for (unsigned int y = 0; y < rows; y++) for (unsigned int x = 0; x < columns; x++) {
        p = packet(&render, 3, 23);
        if (p) { p[1] = (unsigned char)x; p[2] = (unsigned char)y; }
    }
    packet(&render, 1, 13);
    if (!bin.valid || !render.valid || !tile.valid) return 0;
    cache(gpu, gpu->arena, TILE_LIST + LIST_BYTES, HW_CLEAN);
    *bin_bytes = bin.size;
    return render.size;
}

static int faults(V3d* gpu) {
    return (rd(gpu, HUB_INT_STS) & HUB_INT_MMU) || (rd(gpu, MMU_CTL) & MMU_CTL_FAULTS) ||
           (rd(gpu, CTL_INT_STS) & CTL_INT_FAULTS);
}

/* Drop the device's texture and slice caches so it re-reads CPU-written memory. */
static int invalidate_caches(V3d* gpu) {
    wr(gpu, CTL_L2TCACTL, 1U);
    if (settle(gpu, CTL_L2TCACTL, 1U, 0) != HW_OK) return HW_TIMEOUT;
    wr(gpu, CTL_SLCACTL, 0x0f0f0f0fU);
    return HW_OK;
}

static int complete(V3d* gpu, uintptr_t counter, uint32_t previous) {
    uint32_t start = gpu->io->time_us(gpu->io->context);
    for (unsigned int i = 0; i < JOB_POLLS; i++) {
        if (faults(gpu)) return HW_IO;
        if ((rd(gpu, counter) & 255U) != previous) return HW_OK;
        if (gpu->io->time_us(gpu->io->context) - start >= JOB_US) break;
    }
    return HW_TIMEOUT;
}

int v3d_init(V3d* gpu, const HwOps* io, void* table, void* arena,
             uint32_t width, uint32_t height) {
    if (!gpu || !hw_ops_valid(io) || !io->cache || !io->physical || !io->property ||
        !io->owned(io->context) || !table || !arena || !width || !height ||
        width > 800 || height > 800 || (width & 3) || (uint64_t)width * height > 384000 ||
        ((uint64_t)width * height * 4 & 4095U)) return HW_INVALID;
    if (gpu->state) return HW_BUSY;
    uint64_t pt = physical_run(NULL, io, table, 65536, V3D_TABLE_BYTES);
    uint64_t memory = physical_run(NULL, io, arena, 4096, V3D_ARENA_BYTES);
    if (!pt || !memory || ((uintptr_t)table & 65535U) || ((uintptr_t)arena & 4095U) ||
        (pt < memory + V3D_ARENA_BYTES && memory < pt + V3D_TABLE_BYTES)) return HW_INVALID;
    gpu->io = io; gpu->table = table; gpu->arena = arena;
    gpu->width = width; gpu->height = height; gpu->pitch = width * 4; gpu->bytes = width * height * 4;
    gpu->error = gpu->stage = gpu->level = gpu->mapped = gpu->present = 0;
    gpu->detail = 0;
    gpu->state = V3D_BUSY;

    uint32_t values[2] = {FIRMWARE_DOMAIN_V3D, 1};
    int result = io->property(io->context, TAG_SET_DOMAIN_STATE, values, 2);
    if (result != HW_OK) return fail(gpu, V3D_STAGE_POWER, result);
    if (values[0] != FIRMWARE_DOMAIN_V3D || (values[1] & 3U) != 1) return fail(gpu, V3D_STAGE_POWER, HW_PROTOCOL);
    values[0] = FIRMWARE_CLOCK_V3D; values[1] = 1;
    result = io->property(io->context, TAG_SET_CLOCK_STATE, values, 2);
    if (result != HW_OK) return fail(gpu, V3D_STAGE_CLOCK, result);
    if (values[0] != FIRMWARE_CLOCK_V3D || (values[1] & 3U) != 1) return fail(gpu, V3D_STAGE_CLOCK, HW_PROTOCOL);
    values[0] = FIRMWARE_CLOCK_V3D; values[1] = 0;
    result = io->property(io->context, TAG_GET_CLOCK_RATE, values, 2);
    if (result != HW_OK) return fail(gpu, V3D_STAGE_CLOCK, result);
    if (values[0] != FIRMWARE_CLOCK_V3D || !values[1]) return fail(gpu, V3D_STAGE_CLOCK, HW_PROTOCOL);

    /* Full reset cycle. A bridge that never ran may not acknowledge this
     * first stop, so only the restart below is required to succeed. */
    gpu->level = LEVEL_BRIDGE;
    (void)hold_reset(gpu);
    if (hw_delay(io, 10) != HW_OK) return fail(gpu, V3D_STAGE_BRIDGE, HW_TIMEOUT);
    wr(gpu, PM_GRAFX, PM_PASSWORD | rd(gpu, PM_GRAFX) | PM_V3DRSTN);
    if (bridge(gpu, ASB_V3D_M_CTRL, 1) != HW_OK || bridge(gpu, ASB_V3D_S_CTRL, 1) != HW_OK)
        return fail(gpu, V3D_STAGE_BRIDGE, HW_TIMEOUT);

    /* Only V3D 4.2 with one core and an MMU is supported. */
    uint32_t core0 = rd(gpu, CTL_IDENT0), core1 = rd(gpu, CTL_IDENT1);
    uint32_t hub1 = rd(gpu, HUB_IDENT1), hub2 = rd(gpu, HUB_IDENT2);
    if ((core0 & 0xffffffU) != 0x00443356U || (core0 >> 24) != 4 || (core1 & 15U) != 2 ||
        (hub1 & 15U) != 4 || ((hub1 >> 4) & 15U) != 2 || ((hub1 >> 8) & 15U) != 1 ||
        !(hub2 & HUB_IDENT2_WITH_MMU)) return fail(gpu, V3D_STAGE_IDENT, HW_UNSUPPORTED);
    gpu->level = LEVEL_CORE;
    if ((rd(gpu, CSD_STATUS) & 15U) || (rd(gpu, TFU_CS) & 1U)) return fail(gpu, V3D_STAGE_IDLE, HW_BUSY);

    memset(table, 0, V3D_TABLE_BYTES); memset(arena, 0, V3D_ARENA_BYTES);
    /* Lists, tile memory, the surface and its guard page; nothing else. */
    const uint32_t ranges[4][2] = {
        {BIN_LIST, TILE_LIST + LIST_BYTES}, {TILE_ALLOC, TILE_ALLOC + TILE_ALLOC_BYTES},
        {TILE_STATE, SURFACE}, {SURFACE, SURFACE + gpu->bytes + 4096U}
    };
    for (unsigned int range = 0; range < 4; range++)
        for (uint32_t page = ranges[range][0] >> 12; page < ranges[range][1] >> 12; page++)
            gpu->table[(V3D_GPU_BASE >> 12) + page] = (uint32_t)((memory >> 12) + page) | PTE_VALID_WRITEABLE;
    uint32_t* guard = (uint32_t*)(void*)(gpu->arena + SURFACE + gpu->bytes);
    for (unsigned int i = 0; i < 1024; i++) guard[i] = GUARD_WORD;
    cache(gpu, table, V3D_TABLE_BYTES, HW_CLEAN);
    cache(gpu, arena, V3D_ARENA_BYTES, HW_CLEAN_INVALIDATE);
    wr(gpu, CTL_INT_MSK_SET, ~0U); wr(gpu, HUB_INT_MSK_SET, ~0U);
    wr(gpu, CTL_INT_CLR, ~0U); wr(gpu, HUB_INT_CLR, ~0U);
    wr(gpu, CTL_L2TFLSTA, 0); wr(gpu, CTL_L2TFLEND, ~0U);
    gpu->mapped = 1;
    wr(gpu, MMU_PT_PA_BASE, (uint32_t)(pt >> 12));
    wr(gpu, MMU_CTL, MMU_CTL_CONFIG);
    /* Faulting accesses are redirected to the scratch page, never to RAM. */
    wr(gpu, MMU_ILLEGAL_ADDR, (uint32_t)((memory + SCRATCH) >> 12) | (1U << 31));
    wr(gpu, MMUC_CONTROL, MMUC_ENABLE);
    if (mmu_flush(gpu) != HW_OK) return fail(gpu, V3D_STAGE_MMU, HW_TIMEOUT);
    if (rd(gpu, MMU_PT_PA_BASE) != (uint32_t)(pt >> 12) || !(rd(gpu, MMU_CTL) & 1U) || faults(gpu))
        return fail(gpu, V3D_STAGE_MMU, HW_IO);
    gpu->state = V3D_READY;
    return HW_OK;
}

/* One synchronous bin + render job. target == NULL clears the surface. */
static int run(V3d* gpu, int stage, void* target, uint32_t color) {
    if (!gpu || !gpu->io || !gpu->io->owned(gpu->io->context)) return HW_INVALID;
    if (gpu->state != V3D_READY) return gpu->state == V3D_BUSY ? HW_BUSY : HW_OFF;
    unsigned char* surface = gpu->arena + SURFACE;
    uint64_t address = 0;
    if (target && !(address = physical_run(gpu, gpu->io, target, 4096, gpu->bytes))) return HW_UNSUPPORTED;
    gpu->state = V3D_BUSY;
    if (target) {
        if (map_target(gpu, address) != HW_OK) return fail(gpu, stage, HW_TIMEOUT);
        cache(gpu, surface, gpu->bytes, HW_CLEAN);
        cache(gpu, target, gpu->bytes, HW_CLEAN_INVALIDATE);
    } else cache(gpu, surface, gpu->bytes + 4096U, HW_CLEAN_INVALIDATE);
    unsigned int bin_bytes = 0;
    unsigned int render_bytes = build_lists(gpu, target != NULL,
        target ? V3D_TARGET_BASE : V3D_GPU_BASE + SURFACE, color, &bin_bytes);
    if (!render_bytes) return fail(gpu, stage, HW_PROTOCOL);
    if (invalidate_caches(gpu) != HW_OK) return fail(gpu, stage, HW_TIMEOUT);
    uint32_t count = rd(gpu, CLE_BFC) & 255U;
    wr(gpu, CLE_CT0QMA, V3D_GPU_BASE + TILE_ALLOC); wr(gpu, CLE_CT0QMS, TILE_ALLOC_BYTES);
    wr(gpu, CLE_CT0QTS, V3D_GPU_BASE + TILE_STATE + 2);
    wr(gpu, CLE_CT0QBA, V3D_GPU_BASE + BIN_LIST);
    wr(gpu, CLE_CT0QEA, V3D_GPU_BASE + BIN_LIST + bin_bytes);
    int result = complete(gpu, CLE_BFC, count);
    if (result != HW_OK) return fail(gpu, stage, result);
    if (invalidate_caches(gpu) != HW_OK) return fail(gpu, stage, HW_TIMEOUT);
    count = rd(gpu, CLE_RFC) & 255U;
    wr(gpu, CLE_CT1QBA, V3D_GPU_BASE + RENDER_LIST);
    wr(gpu, CLE_CT1QEA, V3D_GPU_BASE + RENDER_LIST + render_bytes);
    result = complete(gpu, CLE_RFC, count);
    if (result != HW_OK) return fail(gpu, stage, result);
    if (faults(gpu)) return fail(gpu, stage, HW_IO);
    if (target) {
        if (map_target(gpu, 0) != HW_OK) return fail(gpu, stage, HW_TIMEOUT);
        cache(gpu, target, gpu->bytes, HW_INVALIDATE);
    } else cache(gpu, surface, gpu->bytes + 4096U, HW_INVALIDATE);
    gpu->state = V3D_READY;
    return HW_OK;
}

int v3d_clear(V3d* gpu, uint32_t rgba) { return run(gpu, V3D_STAGE_CLEAR, NULL, rgba); }

int v3d_present(V3d* gpu, void* target) {
    if (!target) return HW_INVALID;
    return run(gpu, V3D_STAGE_PRESENT, target, 0);
}

uint32_t* v3d_surface(V3d* gpu) {
    if (!gpu || !gpu->io || gpu->state != V3D_READY || !gpu->io->owned(gpu->io->context)) return NULL;
    return (uint32_t*)(void*)(gpu->arena + SURFACE);
}

int v3d_stop(V3d* gpu) {
    if (!gpu || !gpu->io || !gpu->io->owned(gpu->io->context)) return HW_INVALID;
    if (gpu->state == V3D_READY) {
        gpu->state = V3D_BUSY;
        shutdown(gpu);
    }
    return gpu->state;
}

static uint32_t pattern(uint32_t index) { return index * 2654435761U ^ 0x5bd1e995U; }

int v3d_self_test(V3d* gpu, void* target) {
    uint32_t* pixels = v3d_surface(gpu);
    if (!pixels) return gpu && gpu->io && gpu->state < 0 ? HW_OFF : HW_INVALID;
    const uint32_t count = gpu->width * gpu->height, color = 0xff332211U;
    const uint32_t* guard = pixels + count;
    int result = v3d_clear(gpu, color);
    if (result != HW_OK) return result;
    for (uint32_t i = 0; i < count; i++)
        if (pixels[i] != color) return fail(gpu, V3D_STAGE_VERIFY, HW_IO);
    if (target) {
        for (uint32_t i = 0; i < count; i++) pixels[i] = pattern(i);
        result = v3d_present(gpu, target);
        if (result == HW_OK) {
            const volatile uint32_t* shown = target;
            for (uint32_t i = 0; i < count; i++)
                if (shown[i] != pattern(i) || pixels[i] != pattern(i)) return fail(gpu, V3D_STAGE_VERIFY, HW_IO);
            gpu->present = 1;
        } else if (result != HW_UNSUPPORTED) return result;
    }
    result = v3d_clear(gpu, 0);
    if (result != HW_OK) return result;
    for (uint32_t i = 0; i < count; i++)
        if (pixels[i]) return fail(gpu, V3D_STAGE_VERIFY, HW_IO);
    /* Stores must clip to the frame even where it is not tile aligned. */
    for (unsigned int i = 0; i < 1024; i++)
        if (guard[i] != GUARD_WORD) return fail(gpu, V3D_STAGE_VERIFY, HW_IO);
    return HW_OK;
}

const char* v3d_stage_name(int stage) {
    static const char* const names[V3D_STAGE_COUNT] = {
        "none", "power domain", "clock", "reset bridge", "identity", "idle check",
        "MMU setup", "clear job", "present job", "pixel verification"
    };
    return stage >= 0 && stage < V3D_STAGE_COUNT ? names[stage] : "unknown";
}

static int backend_result(V3d* gpu, int result) {
    if (result == HW_OK) return RENDER_DONE;
    /* A rejected call that left the device running touched nothing. */
    return gpu->state < 0 ? RENDER_FAILED : RENDER_SKIP;
}

static int backend_fill(void* context, const RasterFill* fill) {
    V3d* gpu = context;
    if (fill->x || fill->y || fill->width != gpu->width || fill->height != gpu->height) return RENDER_SKIP;
    return backend_result(gpu, v3d_clear(gpu, fill->color));
}

static int backend_present(void* context, const RasterCopy* copy) {
    V3d* gpu = context;
    if (!gpu->present || copy->destination_pitch != gpu->width || copy->source_pitch != gpu->width ||
        copy->width != gpu->width || copy->height != gpu->height) return RENDER_SKIP;
    return backend_result(gpu, v3d_present(gpu, (void*)(uintptr_t)copy->destination));
}

void v3d_render_backend(V3d* gpu, RenderBackend* backend) {
    backend->name = "V3D";
    backend->context = gpu;
    backend->surface = v3d_surface(gpu);
    backend->fill = backend_fill;
    backend->present = backend_present;
    backend->retired = NULL;
}
