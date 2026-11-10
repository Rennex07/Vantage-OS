#include <vantage/drivers/v3d.h>
#include "v3d_model.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

#define MAX_BYTES (800U * 480U * 4U)
static unsigned char table_storage[V3D_TABLE_BYTES + 65535U];
static unsigned char* table;
static HW_ALIGN(4096) unsigned char arena[V3D_ARENA_BYTES];
static HW_ALIGN(4096) unsigned char target[MAX_BYTES + 4096U];
static HW_ALIGN(4096) unsigned char stranger[8192];
static V3dModel m;
static HwOps io;
static V3d gpu;

static void fresh(void) {
    v3d_model_free(&m);
    memset(&gpu, 0, sizeof(gpu));
    memset(arena, 0xee, sizeof(arena));
    memset(target, 0x77, sizeof(target));
    v3d_model_init(&m, table, arena);
    v3d_model_add(&m, target, sizeof(target), 0x3000000U);
    io = v3d_model_ops(&m);
}

static void start(uint32_t width, uint32_t height) {
    fresh();
    CHECK(v3d_init(&gpu, &io, table, arena, width, height) == HW_OK && gpu.state == V3D_READY);
    CHECK(v3d_self_test(&gpu, target) == HW_OK && gpu.present && v3d_model_quiet(&m));
}

/* A stopped device is in reset behind stopped bridges with its domain off,
 * and the context refuses everything afterwards. */
static void check_stopped(int state, int stage, int error) {
    CHECK(gpu.state == state && gpu.stage == stage && gpu.error == error);
    CHECK(!m.active && !m.domain_on);
    if (state == V3D_STOPPED) CHECK(v3d_model_off(&m));
    CHECK(v3d_clear(&gpu, 0) == HW_OFF && v3d_present(&gpu, target) == HW_OFF);
    CHECK(!v3d_surface(&gpu) && v3d_self_test(&gpu, NULL) == HW_OFF && v3d_stop(&gpu) == state);
    CHECK(v3d_init(&gpu, &io, table, arena, 64, 64) == HW_BUSY);
}

static void expect_init_failure(int stage, int error) {
    unsigned int before = m.register_writes;
    CHECK(v3d_init(&gpu, &io, table, arena, 64, 64) == error);
    if (stage <= V3D_STAGE_CLOCK) CHECK(m.register_writes == before && !gpu.level);
    CHECK(!m.bin_jobs && !m.render_jobs);
    check_stopped(V3D_STOPPED, stage, error);
}

static uint32_t pixel(const unsigned char* base, uint32_t index) {
    uint32_t value;
    memcpy(&value, base + (size_t)index * 4, 4);
    return value;
}

static void test_bring_up_and_mappings(void) {
    fresh();
    CHECK(v3d_init(&gpu, &io, table, arena, 800, 480) == HW_OK);
    CHECK(m.domain_on && m.clock_on && m.resets == 0 && m.tlb_flushes == 1 && m.property_calls == 3);
    /* Only lists, tile memory, the surface and its guard page are mapped. */
    const uint32_t* pte = (const uint32_t*)(const void*)table;
    const uint32_t first = V3D_GPU_BASE >> 12, surface_end = (0x40000U + MAX_BYTES) >> 12;
    for (uint32_t page = 0; page < V3D_TABLE_BYTES / 4; page++) {
        uint32_t offset = page - first;
        int mapped = page >= first && (offset < 3 || (offset >= 0x10 && offset <= surface_end));
        CHECK(pte[page] == (mapped ? (0x30000000U | (0x2000000U >> 12) | offset) : 0));
    }
    CHECK(!memcmp(table, m.region[0].ram, V3D_TABLE_BYTES));
    CHECK(m.illegal == ((1U << 31) | ((0x2000000U + V3D_ARENA_BYTES - 4096U) >> 12)));

    /* Counters start at 255, so the first jobs cross the 8-bit wrap. */
    CHECK(v3d_self_test(&gpu, target) == HW_OK && gpu.present);
    CHECK(m.bin_jobs == 3 && m.render_jobs == 3 && m.bfc == 2 && m.rfc == 2 && !m.mmu_faults);
    CHECK(m.tlb_flushes == 3 && v3d_model_quiet(&m));
    uint32_t* pixels = v3d_surface(&gpu);
    CHECK(pixels == (uint32_t*)(void*)(arena + 0x40000));
    for (uint32_t i = 0; i < 800 * 480; i++) CHECK(!pixels[i]);

    /* The CPU draws between jobs; a present shows exactly that frame. */
    for (uint32_t i = 0; i < 800 * 480; i++) pixels[i] = i * 40503U + 7;
    CHECK(v3d_present(&gpu, target) == HW_OK && v3d_model_quiet(&m));
    CHECK(!memcmp(target, pixels, MAX_BYTES));
    for (uint32_t i = 0; i < 4096; i++) CHECK(target[MAX_BYTES + i] == 0x77);
    CHECK(v3d_clear(&gpu, 0x80c0ffeeU) == HW_OK);
    for (uint32_t i = 0; i < 800 * 480; i++) CHECK(pixels[i] == 0x80c0ffeeU);
    CHECK(pixel(target, 5) == 5 * 40503U + 7);

    CHECK(v3d_stop(&gpu) == V3D_STOPPED && m.resets == 1);
    check_stopped(V3D_STOPPED, V3D_STAGE_NONE, 0);
}

static void test_rejected_requests(void) {
    fresh();
    CHECK(v3d_init(NULL, &io, table, arena, 64, 64) == HW_INVALID);
    CHECK(v3d_init(&gpu, NULL, table, arena, 64, 64) == HW_INVALID);
    CHECK(v3d_init(&gpu, &io, NULL, arena, 64, 64) == HW_INVALID);
    CHECK(v3d_init(&gpu, &io, table, NULL, 64, 64) == HW_INVALID);
    CHECK(v3d_init(&gpu, &io, table + 4096, arena, 64, 64) == HW_INVALID);
    CHECK(v3d_init(&gpu, &io, table, arena + 64, 64, 64) == HW_INVALID);
    CHECK(v3d_init(&gpu, &io, table, stranger, 64, 64) == HW_INVALID);
    CHECK(v3d_init(&gpu, &io, table, arena, 0, 64) == HW_INVALID);
    CHECK(v3d_init(&gpu, &io, table, arena, 66, 64) == HW_INVALID);
    CHECK(v3d_init(&gpu, &io, table, arena, 804, 480) == HW_INVALID);
    CHECK(v3d_init(&gpu, &io, table, arena, 64, 63) == HW_INVALID); /* Not whole pages. */
    m.owner = 0;
    CHECK(v3d_init(&gpu, &io, table, arena, 64, 64) == HW_INVALID);
    CHECK(!gpu.state && !m.register_writes && !m.property_calls && !m.cache_calls);

    start(64, 64);
    unsigned int writes = m.register_writes, calls = m.cache_calls;
    m.owner = 0;
    CHECK(v3d_clear(&gpu, 1) == HW_INVALID && v3d_present(&gpu, target) == HW_INVALID);
    CHECK(!v3d_surface(&gpu) && v3d_self_test(&gpu, target) == HW_INVALID && v3d_stop(&gpu) == HW_INVALID);
    m.owner = 1;
    /* Targets the device cannot be lent leave it untouched and usable. */
    CHECK(v3d_present(&gpu, NULL) == HW_INVALID);
    CHECK(v3d_present(&gpu, stranger) == HW_UNSUPPORTED);
    CHECK(v3d_present(&gpu, target + 64) == HW_UNSUPPORTED);
    CHECK(v3d_present(&gpu, arena + 0x100000) == HW_UNSUPPORTED);
    CHECK(v3d_present(&gpu, table) == HW_UNSUPPORTED);
    CHECK(v3d_present(&gpu, target + sizeof(target) - 8192) == HW_UNSUPPORTED); /* Runs off the buffer. */
    CHECK(gpu.state == V3D_READY && m.register_writes == writes && m.cache_calls == calls);
    CHECK(v3d_present(&gpu, target + 4096) == HW_OK && v3d_model_quiet(&m));
}

static void test_init_failures(void) {
    static const uint32_t tags[3] = {0x00038030U, 0x00038001U, 0x00030002U};
    static const int results[3] = {HW_TIMEOUT, HW_PROTOCOL, HW_OFF};
    for (unsigned int tag = 0; tag < 3; tag++) {
        int stage = tag ? V3D_STAGE_CLOCK : V3D_STAGE_POWER;
        for (unsigned int result = 0; result < 3; result++) {
            fresh(); m.fail_tag = tags[tag]; m.fail_result = results[result];
            expect_init_failure(stage, results[result]);
        }
        fresh(); m.absent_tag = tags[tag];
        expect_init_failure(stage, HW_PROTOCOL);
    }
    fresh(); m.bridge_stuck = 1;
    expect_init_failure(V3D_STAGE_BRIDGE, HW_TIMEOUT);
    /* A stopped timer still ends every wait through the poll bound. */
    fresh(); m.bridge_stuck = 1; m.clock_step = 0;
    expect_init_failure(V3D_STAGE_BRIDGE, HW_TIMEOUT);

    static const uint32_t wrong[6][2] = {
        {0, 0x03443356U}, {0, 0x04443357U}, {1, 1}, {2, 0x114U}, {2, 0x224U}, {3, 0}
    };
    for (unsigned int i = 0; i < 6; i++) {
        fresh(); m.ident[wrong[i][0]] = wrong[i][1];
        expect_init_failure(V3D_STAGE_IDENT, HW_UNSUPPORTED);
    }
    fresh(); memset(m.ident, 0, sizeof(m.ident)); /* Nothing answers at the address. */
    expect_init_failure(V3D_STAGE_IDENT, HW_UNSUPPORTED);
    fresh(); m.csd_busy = 1;
    expect_init_failure(V3D_STAGE_IDLE, HW_BUSY);
    CHECK(!gpu.mapped && gpu.detail == 0x7000U);
    fresh(); m.tlb_stuck = 1;
    expect_init_failure(V3D_STAGE_MMU, HW_TIMEOUT);
    fresh(); m.bad_table_readback = 1;
    expect_init_failure(V3D_STAGE_MMU, HW_IO);
    CHECK(gpu.mapped && m.resets == 1);

    /* Once the table address was handed over, an unconfirmed stop keeps the
     * buffers quarantined; before that they are always returned. */
    fresh(); m.bad_table_readback = 1; m.reset_stuck = 1;
    CHECK(v3d_init(&gpu, &io, table, arena, 64, 64) == HW_IO);
    check_stopped(V3D_QUARANTINED, V3D_STAGE_MMU, HW_IO);
    fresh(); m.csd_busy = 1; m.reset_stuck = 1;
    CHECK(v3d_init(&gpu, &io, table, arena, 64, 64) == HW_BUSY && gpu.state == V3D_STOPPED);
}

static void test_self_test_failures(void) {
    fresh(); CHECK(v3d_init(&gpu, &io, table, arena, 64, 64) == HW_OK);
    m.corrupt = 1;
    CHECK(v3d_self_test(&gpu, target) == HW_IO);
    check_stopped(V3D_STOPPED, V3D_STAGE_VERIFY, HW_IO);

    /* A store that ignores the frame height lands in the guard page. */
    fresh(); CHECK(v3d_init(&gpu, &io, table, arena, 64, 48) == HW_OK);
    m.overrun = 1;
    CHECK(v3d_self_test(&gpu, NULL) == HW_IO && !m.mmu_faults);
    check_stopped(V3D_STOPPED, V3D_STAGE_VERIFY, HW_IO);
    /* The same overrun on a present leaves the lent pages and faults. */
    start(64, 48);
    m.overrun = 1;
    CHECK(v3d_present(&gpu, target) == HW_IO && m.mmu_faults);
    check_stopped(V3D_STOPPED, V3D_STAGE_PRESENT, HW_IO);

    fresh(); CHECK(v3d_init(&gpu, &io, table, arena, 64, 64) == HW_OK);
    m.l2t_stuck = 1;
    CHECK(v3d_self_test(&gpu, target) == HW_TIMEOUT);
    check_stopped(V3D_STOPPED, V3D_STAGE_CLEAR, HW_TIMEOUT);

    /* Without a usable target only clears are verified and enabled. */
    fresh(); CHECK(v3d_init(&gpu, &io, table, arena, 160, 96) == HW_OK);
    CHECK(v3d_self_test(&gpu, stranger) == HW_OK && !gpu.present && m.render_jobs == 2);
    CHECK(v3d_self_test(&gpu, NULL) == HW_OK && !gpu.present);
    CHECK(v3d_self_test(&gpu, target) == HW_OK && gpu.present);
}

static void test_job_failures(void) {
    /* Binner never completes; the timeout also survives timer wrap. */
    start(160, 96);
    m.stall_bin = 1; m.clock = 0xffff0000U;
    CHECK(v3d_clear(&gpu, 5) == HW_TIMEOUT && m.resets == 1);
    check_stopped(V3D_STOPPED, V3D_STAGE_CLEAR, HW_TIMEOUT);

    /* Rendering stops halfway: the CPU can see the partial output. */
    start(160, 96);
    uint32_t* pixels = v3d_surface(&gpu);
    for (uint32_t i = 0; i < 160 * 96; i++) pixels[i] = 0x01010101U * (i % 251);
    m.stall_render = 1;
    CHECK(v3d_clear(&gpu, 0xabcdef01U) == HW_TIMEOUT);
    check_stopped(V3D_STOPPED, V3D_STAGE_CLEAR, HW_TIMEOUT);
    CHECK(pixel(arena + 0x40000, 0) == 0xabcdef01U && pixel(arena + 0x40000, 160 * 96 - 1) != 0xabcdef01U);

    start(160, 96);
    pixels = v3d_surface(&gpu);
    for (uint32_t i = 0; i < 160 * 96; i++) pixels[i] = i ^ 0x5a5a5a5aU;
    m.stall_render = 1;
    CHECK(v3d_present(&gpu, target) == HW_TIMEOUT);
    check_stopped(V3D_STOPPED, V3D_STAGE_PRESENT, HW_TIMEOUT);
    /* The source surface is intact for the CPU to present instead. */
    for (uint32_t i = 0; i < 160 * 96; i++) CHECK(pixel(arena + 0x40000, i) == (i ^ 0x5a5a5a5aU));
    CHECK(pixel(target, 0) == 0x5a5a5a5aU && pixel(target, 160 * 96 - 1) != ((160 * 96 - 1) ^ 0x5a5a5a5aU));

    /* MMU fault status ends the job without waiting for the timeout. */
    start(160, 96);
    m.fault_render = 1;
    uint32_t before = m.clock;
    CHECK(v3d_clear(&gpu, 9) == HW_IO && m.clock - before < 100000U);
    check_stopped(V3D_STOPPED, V3D_STAGE_CLEAR, HW_IO);
    CHECK(gpu.detail == 0x7000U);

    /* Stop not acknowledged: the device may still own its memory. */
    start(160, 96);
    m.stall_bin = 1; m.bridge_stuck = 1;
    CHECK(v3d_present(&gpu, target) == HW_TIMEOUT);
    check_stopped(V3D_QUARANTINED, V3D_STAGE_PRESENT, HW_TIMEOUT);
    start(160, 96);
    m.stall_render = 1; m.reset_stuck = 1;
    CHECK(v3d_clear(&gpu, 9) == HW_TIMEOUT);
    check_stopped(V3D_QUARANTINED, V3D_STAGE_CLEAR, HW_TIMEOUT);
    start(160, 96);
    m.bridge_stuck = 1;
    CHECK(v3d_stop(&gpu) == V3D_QUARANTINED);

    /* A TLB flush that never finishes fails the present before any job. */
    start(160, 96);
    unsigned int jobs = m.render_jobs;
    m.tlb_stuck = 1;
    CHECK(v3d_present(&gpu, target) == HW_TIMEOUT && m.render_jobs == jobs);
    check_stopped(V3D_STOPPED, V3D_STAGE_PRESENT, HW_TIMEOUT);
}

static void test_sustained_jobs(void) {
    start(160, 96);
    uint32_t* pixels = v3d_surface(&gpu);
    uint32_t state = 99;
    for (unsigned int round = 0; round < 300; round++) {
        state = state * 1664525U + 1013904223U;
        if (round % 3 == 0) {
            CHECK(v3d_clear(&gpu, state) == HW_OK);
            for (uint32_t i = 0; i < 160 * 96; i++) CHECK(pixels[i] == state);
        } else {
            for (uint32_t i = 0; i < 160 * 96; i += 7) pixels[i] = state + i;
            unsigned char* page = target + (round % 2) * 65536U;
            page[round * 37U % 61440U] ^= 0xffU; /* Stale CPU data must not survive the job. */
            CHECK(v3d_present(&gpu, page) == HW_OK && !memcmp(page, pixels, 160 * 96 * 4));
        }
        CHECK(v3d_model_quiet(&m) && gpu.state == V3D_READY);
    }
    CHECK(m.render_jobs == 303 && m.bin_jobs == 303 && !m.mmu_faults && m.rfc == (255 + 303) % 256);
}

int main(void) {
    table = (unsigned char*)(((uintptr_t)table_storage + 65535U) & ~(uintptr_t)65535U);
    CHECK(v3d_stage_name(V3D_STAGE_IDENT)[0] == 'i' && v3d_stage_name(-1)[0] == 'u' &&
          v3d_stage_name(V3D_STAGE_COUNT)[0] == 'u');
    test_bring_up_and_mappings();
    test_rejected_requests();
    test_init_failures();
    test_self_test_failures();
    test_job_failures();
    test_sustained_jobs();
    v3d_model_free(&m);
    puts("PASS: V3D bring-up, per-step failure quiescence, private and per-job mappings, cache/TLB "
         "ordering, counter and timer wrap, job timeout/fault reset, quarantine");
    return 0;
}
