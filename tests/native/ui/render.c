#include <vantage/kernel/render.h>
#include <vantage/drivers/v3d.h>
#include "v3d_model.h"
#include <vantage/kernel/parallel_port.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

/* Single-owner port: the dispatcher stays in its serial slice. Threaded
 * raster batches are covered by the simulator and scheduler suites. */
static int expect_halt;
uint32_t parallel_load(const uint32_t* value) { return *value; }
void parallel_store(uint32_t* value, uint32_t next) { *value = next; }
unsigned int parallel_core_id(void) { return 0; }
uint32_t parallel_time_us(void) { return 0; }
void parallel_idle(void) {}
void parallel_wake(void) {}
void parallel_completion_barrier(void) {}
void parallel_port_halt(const char* message) {
    if (expect_halt) { puts("PASS: render backend seam, V3D/CPU pixel equivalence, frame-preserving runtime fallback and re-entry halt"); exit(0); }
    fprintf(stderr, "%s", message);
    exit(1);
}

#define W 37U
#define H 23U
#define PITCH 41U
static uint32_t cpu_pixels[W * H], fake_pixels[W * H], oracle[W * H];
static uint32_t shown[PITCH * H], expected[PITCH * H];

typedef struct { Renderer* renderer; int fills, presents, retired, fail_fill, fail_present, scribble; } Fake;

/* Accepts whole-surface fills and tight-pitch presents, like a device that
 * only implements full-frame operations. */
static int fake_fill(void* context, const RasterFill* fill) {
    Fake* fake = context;
    CHECK(fake->renderer->busy && fill->pixels == fake_pixels && fill->pitch == W);
    if (fill->x || fill->y || fill->width != W || fill->height != H) return RENDER_SKIP;
    if (fake->fail_fill) {
        /* A stopped job leaves its target partially written. */
        for (unsigned int i = 0; i < W * H; i += 3) fake_pixels[i] = 0xdeadbeefU;
        return RENDER_FAILED;
    }
    for (unsigned int i = 0; i < W * H; i++) fake_pixels[i] = fill->color;
    fake->fills++;
    return RENDER_DONE;
}

static int fake_present(void* context, const RasterCopy* copy) {
    Fake* fake = context;
    CHECK(fake->renderer->busy && copy->source == fake_pixels && copy->width == W && copy->height == H);
    if (copy->destination_pitch != W) return RENDER_SKIP;
    if (fake->fail_present) {
        for (unsigned int i = 0; i < W; i++) copy->destination[i] = 0xdeadbeefU;
        return RENDER_FAILED;
    }
    for (unsigned int y = 0; y < H; y++)
        for (unsigned int x = 0; x < W; x++)
            copy->destination[y * copy->destination_pitch + x] = fake_pixels[y * W + x];
    fake->presents++;
    return RENDER_DONE;
}

static void fake_retired(void* context) {
    Fake* fake = context;
    /* The renderer must already have left the surface. */
    CHECK(fake->renderer->pixels == cpu_pixels && !fake->renderer->backend);
    fake->retired++;
    if (fake->scribble) memset(fake_pixels, 0x5a, sizeof(fake_pixels));
}

static uint32_t next(uint32_t* state) {
    *state = *state * 1664525U + 1013904223U;
    return *state >> 8;
}

static void oracle_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!w || !h || x >= W || y >= H || w > W - x || h > H - y) return;
    for (uint32_t row = y; row < y + h; row++)
        for (uint32_t column = x; column < x + w; column++) oracle[row * W + column] = color;
}

/* One deterministic mixed operation, mirrored into the oracle. */
static void step(Renderer* renderer, uint32_t* state) {
    uint32_t kind = next(state) % 8, color = next(state) * 2654435761U;
    if (kind == 0) {
        render_fill(renderer, 0, 0, W, H, color);
        oracle_fill(0, 0, W, H, color);
    } else if (kind < 4) {
        uint32_t x = next(state) % (W + 2), y = next(state) % (H + 2);
        uint32_t w = next(state) % (W + 2), h = next(state) % (H + 2);
        render_fill(renderer, x, y, w, h, color);
        oracle_fill(x, y, w, h, color);
    } else if (kind < 7) {
        uint32_t x = next(state) % (W + 2), y = next(state) % (H + 2);
        render_pixel(renderer, x, y, color);
        if (x < W && y < H) oracle[y * W + x] = color;
    } else {
        memset(shown, 0xa5, sizeof(shown));
        memset(expected, 0xa5, sizeof(expected));
        render_present(renderer, shown, PITCH);
        for (unsigned int y = 0; y < H; y++) memcpy(expected + y * PITCH, oracle + y * W, W * sizeof(uint32_t));
        CHECK(!memcmp(shown, expected, sizeof(shown)));
    }
    CHECK(!memcmp(renderer->pixels, oracle, sizeof(oracle)) && !renderer->busy);
}

/* The production V3D engine and adapter over the device model, compared
 * against CPU rendering after every operation. */
#define GW 160U
#define GH 96U
#define GBYTES (GW * GH * sizeof(uint32_t))
static unsigned char gpu_table_storage[V3D_TABLE_BYTES + 65535U];
static HW_ALIGN(4096) unsigned char gpu_arena[V3D_ARENA_BYTES];
static HW_ALIGN(4096) uint32_t gpu_pages[2][GW * GH];
static uint32_t reference[GW * GH], reference_page[GW * GH], standby[GW * GH];
static uint32_t padded[2][(GW + 5) * GH];
static int retirements;

static void count_retirement(void* context) {
    V3d* gpu = context;
    CHECK(gpu->state < 0);
    retirements++;
}

static void mirrored_present(Renderer* cpu, Renderer* accelerated, unsigned int page) {
    memset(gpu_pages[page], 0x11, GBYTES);
    render_present(accelerated, gpu_pages[page], GW);
    render_present(cpu, reference_page, GW);
    CHECK(!memcmp(gpu_pages[page], reference_page, GBYTES));
}

static void mirrored_step(Renderer* cpu, Renderer* accelerated, uint32_t* state) {
    uint32_t kind = next(state) % 16, color = next(state) * 2654435761U;
    if (kind < 2) {
        render_fill(cpu, 0, 0, GW, GH, color);
        render_fill(accelerated, 0, 0, GW, GH, color);
    } else if (kind < 7) {
        uint32_t x = next(state) % (GW + 2), y = next(state) % (GH + 2);
        uint32_t w = next(state) % (GW + 2), h = next(state) % (GH + 2);
        render_fill(cpu, x, y, w, h, color);
        render_fill(accelerated, x, y, w, h, color);
    } else if (kind < 12) {
        uint32_t x = next(state) % GW, y = next(state) % GH;
        render_pixel(cpu, x, y, color);
        render_pixel(accelerated, x, y, color);
    } else if (kind < 15) {
        mirrored_present(cpu, accelerated, kind & 1);
    } else {
        /* Row padding is outside the GPU present contract. */
        memset(padded, 0x22, sizeof(padded));
        render_present(cpu, padded[0], GW + 5);
        render_present(accelerated, padded[1], GW + 5);
        CHECK(!memcmp(padded[0], padded[1], sizeof(padded[0])));
    }
    CHECK(!memcmp(cpu->pixels, accelerated->pixels, GBYTES) && !accelerated->busy);
}

static void gpu_session(int failure) {
    V3dModel model;
    V3d gpu;
    RenderBackend backend;
    Renderer cpu, accelerated;
    uint32_t state = 1000 + (uint32_t)failure;
    unsigned char* table = (unsigned char*)(((uintptr_t)gpu_table_storage + 65535U) & ~(uintptr_t)65535U);
    memset(&gpu, 0, sizeof(gpu));
    v3d_model_init(&model, table, gpu_arena);
    v3d_model_add(&model, gpu_pages, sizeof(gpu_pages), 0x3000000U);
    HwOps io = v3d_model_ops(&model);
    retirements = 0;

    /* A frame drawn before the backend exists must survive the switch. */
    memset(reference, 0, sizeof(reference));
    memset(standby, 0, sizeof(standby));
    render_init(&cpu, reference, GW, GH);
    render_init(&accelerated, standby, GW, GH);
    for (unsigned int i = 0; i < 50; i++) mirrored_step(&cpu, &accelerated, &state);
    CHECK(v3d_init(&gpu, &io, table, gpu_arena, GW, GH) == HW_OK);
    CHECK(v3d_self_test(&gpu, gpu_pages[1]) == HW_OK && gpu.present);
    v3d_render_backend(&gpu, &backend);
    backend.retired = count_retirement;
    CHECK(backend.surface && render_attach(&accelerated, &backend));
    CHECK(!memcmp(reference, accelerated.pixels, GBYTES));

    unsigned int jobs = model.render_jobs;
    for (unsigned int i = 0; i < 400; i++) {
        mirrored_step(&cpu, &accelerated, &state);
        CHECK(v3d_model_quiet(&model));
    }
    /* Full fills and tight presents ran on the device; nothing faulted. */
    CHECK(model.render_jobs > jobs + 60 && !model.mmu_faults && !retirements);
    CHECK(accelerated.backend == &backend && gpu.state == V3D_READY);
    if (failure < 0) {
        CHECK(v3d_stop(&gpu) == V3D_STOPPED);
        v3d_model_free(&model);
        return;
    }

    for (uint32_t i = 0; i < GW * GH; i += 5) {
        render_pixel(&cpu, i % GW, i / GW, i * 97U);
        render_pixel(&accelerated, i % GW, i / GW, i * 97U);
    }
    int quarantined = failure == 3;
    if (failure == 0 || failure == 1) model.stall_render = 1;
    if (failure == 2) model.fault_render = 1;
    if (failure == 3) model.stall_bin = model.bridge_stuck = 1;
    if (failure == 4) model.tlb_stuck = 1;
    if (failure == 0 || failure == 3) {
        render_fill(&cpu, 0, 0, GW, GH, 0x00c0ffeeU);
        render_fill(&accelerated, 0, 0, GW, GH, 0x00c0ffeeU);
    } else mirrored_present(&cpu, &accelerated, 0);
    CHECK(retirements == 1 && !accelerated.backend && accelerated.pixels == standby);
    CHECK(!memcmp(reference, standby, GBYTES));
    CHECK(gpu.state == (quarantined ? V3D_QUARANTINED : V3D_STOPPED));
    CHECK(quarantined || v3d_model_off(&model));

    /* The session continues on the CPU without touching the device again. */
    unsigned int writes = model.register_writes, calls = model.cache_calls;
    for (unsigned int i = 0; i < 300; i++) mirrored_step(&cpu, &accelerated, &state);
    mirrored_present(&cpu, &accelerated, 1);
    CHECK(retirements == 1 && model.register_writes == writes && model.cache_calls == calls);
    v3d_model_free(&model);
}

int main(void) {    Renderer renderer;
    uint32_t state = 7;
    render_init(&renderer, cpu_pixels, W, H);
    for (unsigned int i = 0; i < 2000; i++) step(&renderer, &state);

    /* Attaching moves the live frame; supported operations reach the backend. */
    Fake fake = {&renderer, 0, 0, 0, 0, 0, 0};
    RenderBackend backend = {"fake", &fake, fake_pixels, fake_fill, fake_present, fake_retired};
    CHECK(render_attach(&renderer, &backend) && renderer.pixels == fake_pixels);
    CHECK(!render_attach(&renderer, &backend));
    CHECK(!memcmp(fake_pixels, oracle, sizeof(oracle)));
    for (unsigned int i = 0; i < 2000; i++) step(&renderer, &state);
    CHECK(fake.fills > 100 && !fake.presents && !fake.retired); /* Padded pitch stays on the CPU. */
    render_present(&renderer, shown, W);
    CHECK(fake.presents == 1 && !memcmp(shown, oracle, sizeof(oracle)));

    /* A failed present must not lose the frame or leave the target partial. */
    fake.fail_present = fake.scribble = 1;
    render_pixel(&renderer, 3, 4, 0x11223344U);
    oracle[4 * W + 3] = 0x11223344U;
    memset(shown, 0, sizeof(shown));
    render_present(&renderer, shown, W);
    CHECK(fake.retired == 1 && renderer.pixels == cpu_pixels && !renderer.backend);
    CHECK(!memcmp(shown, oracle, sizeof(oracle)) && !memcmp(cpu_pixels, oracle, sizeof(oracle)));
    for (unsigned int i = 0; i < 500; i++) step(&renderer, &state);
    CHECK(fake.retired == 1 && fake.presents == 1);

    /* A failed clear is repeated on the CPU after the frame is copied out. */
    fake.fail_present = fake.scribble = 0;
    fake.fail_fill = 1;
    CHECK(render_attach(&renderer, &backend));
    render_fill(&renderer, 1, 1, 5, 5, 0x01020304U);
    oracle_fill(1, 1, 5, 5, 0x01020304U);
    CHECK(renderer.backend == &backend && !memcmp(fake_pixels, oracle, sizeof(oracle)));
    render_fill(&renderer, 0, 0, W, H, 0x0a0b0c0dU);
    oracle_fill(0, 0, W, H, 0x0a0b0c0dU);
    CHECK(fake.retired == 2 && !renderer.backend && !memcmp(cpu_pixels, oracle, sizeof(oracle)));

    /* Out-of-range requests never reach memory. */
    render_fill(&renderer, W - 1, 0, 2, 1, 1);
    render_fill(&renderer, 0, H, 1, 1, 1);
    render_fill(&renderer, 0, 0, 0xffffffffU, 1, 1);
    render_pixel(&renderer, W, 0, 1);
    render_present(&renderer, NULL, W);
    render_present(&renderer, shown, W - 1);
    CHECK(!memcmp(cpu_pixels, oracle, sizeof(oracle)));

    for (int failure = -1; failure <= 4; failure++) gpu_session(failure);

    /* Drawing while an operation is in flight is re-entry and must halt. */
    expect_halt = 1;
    renderer.busy = 1;
    render_pixel(&renderer, 0, 0, 1);
    return 1;
}
