#define SDL_MAIN_HANDLED
#include "SDL2/SDL.h"
#include <vantage/kernel/parallel_port.h>
#include <vantage/kernel/raster.h>
#include <stdio.h>
#include <stdlib.h>

static SDL_TLSID core_key;
static SDL_atomic_t stop[PARALLEL_CORES];
static SDL_Thread* threads[PARALLEL_CORES];
static int expected_timeout;

uint32_t parallel_load(const uint32_t* value) {
    return (uint32_t)SDL_AtomicGet((SDL_atomic_t*)(void*)value);
}

void parallel_store(uint32_t* value, uint32_t next) {
    SDL_AtomicSet((SDL_atomic_t*)(void*)value, (int)next);
}

unsigned int parallel_core_id(void) {
    return (unsigned int)(uintptr_t)SDL_TLSGet(core_key);
}

uint32_t parallel_time_us(void) {
    uint64_t ticks = SDL_GetPerformanceCounter();
    uint64_t frequency = SDL_GetPerformanceFrequency();
    return (uint32_t)((ticks / frequency) * 1000000 +
                      (ticks % frequency) * 1000000 / frequency);
}

void parallel_idle(void) { SDL_Delay(0); }
void parallel_wake(void) {}
void parallel_completion_barrier(void) { SDL_MemoryBarrierRelease(); }

void parallel_port_halt(const char* message) {
    fprintf(stderr, "%s", message);
    exit(expected_timeout && strstr(message, "core 3 job timed out") ? 0 : 1);
}

static int worker(void* argument) {
    unsigned int core = (unsigned int)(uintptr_t)argument;
    SDL_TLSSet(core_key, argument, NULL);
    parallel_worker_online(core);
    while (!SDL_AtomicGet(&stop[core])) {
        if (!parallel_worker_poll(core)) parallel_idle();
    }
    return 0;
}

static void require(int condition, const char* reason) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", reason);
        exit(1);
    }
}

typedef struct {
    uint32_t seen[PARALLEL_CORES];
    uint32_t sum[PARALLEL_CORES];
    uint32_t nested_rejected[PARALLEL_CORES];
    uint32_t parts, seed;
} Probe;

static void probe(unsigned int part, unsigned int parts, void* context) {
    Probe* p = context;
    p->seen[part] = parallel_core_id() + 1;
    p->nested_rejected[part] = !parallel_run(probe, context);
    for (uint32_t i = part; i < 4096; i += parts) p->sum[part] += i ^ p->seed;
    if (part == 0) p->parts = parts;
}

static void test_raster(void) {
    uint32_t serial[13 * 19], parallel[13 * 19], expected[13 * 19], copied[17 * 19];
    for (unsigned int width = 0; width <= 9; width++) {
        for (unsigned int height = 0; height <= 17; height++) {
            /* Coordinate-based oracle, independent of the raster callbacks. */
            for (unsigned int y = 0; y < 19; y++) {
                for (unsigned int x = 0; x < 13; x++) {
                    unsigned int i = y * 13 + x;
                    serial[i] = parallel[i] = 0x13579;
                    expected[i] = x >= 2 && x < 2 + width && y >= 1 && y < 1 + height
                                  ? 0xabcdef : 0x13579;
                }
            }
            for (unsigned int i = 0; i < 17 * 19; i++) copied[i] = 0x2468;
            RasterFill a = { serial, 13, 2, 1, width, height, 0xabcdef };
            RasterFill b = { parallel, 13, 2, 1, width, height, 0xabcdef };
            raster_fill_rows(0, 1, &a);
            require(parallel_run(raster_fill_rows, &b), "parallel raster submission");
            require(!memcmp(serial, expected, sizeof(expected)), "serial fill differs from expected pixels");
            require(!memcmp(parallel, expected, sizeof(expected)), "parallel fill differs from expected pixels");
            require(!memcmp(serial, parallel, sizeof(serial)), "parallel fill differs from serial");
            RasterCopy c = { parallel, copied, 13, 17, 13, height };
            require(parallel_run(raster_copy_rows, &c), "parallel copy submission");
            for (unsigned int y = 0; y < 19; y++) {
                for (unsigned int x = 0; x < 17; x++) {
                    uint32_t pixel = y < height && x < 13 ? expected[y * 13 + x] : 0x2468;
                    require(copied[y * 17 + x] == pixel, "copy differs from expected pixels/padding");
                }
            }
        }
    }
}

int main(int argc, char** argv) {
    SDL_SetMainReady();
    require(SDL_Init(SDL_INIT_TIMER) == 0, "SDL timer setup");
    core_key = SDL_TLSCreate();
    require(core_key != 0, "thread-local core IDs");
    SDL_TLSSet(core_key, NULL, NULL);
    Probe serial = { {0}, {0}, {0}, 0, 11 };
    require(parallel_run(probe, &serial) && serial.parts == 1 && serial.seen[0] == 1,
            "pre-init serial fallback");
    require(serial.nested_rejected[0] && !parallel_run(NULL, NULL), "invalid submissions");
    require(!parallel_self_test(), "self-test cannot pass without workers");

    int missing = argc > 1 && !strcmp(argv[1], "--missing");
    for (unsigned int core = 1; core < (missing ? 3u : 4u); core++) {
        threads[core] = SDL_CreateThread(worker, "parallel-worker", (void*)(uintptr_t)core);
        require(threads[core] != NULL, "create worker thread");
    }
    if (missing) {
        require(!parallel_init(), "missing worker must fail initialization");
    } else {
        require(parallel_init(), "all workers online");
        require(!parallel_init(), "initialization is one-shot");
        require(parallel_self_test(), "four simultaneously in-flight callbacks and checksums");
        for (unsigned int round = 0; round < 256; round++) {
            Probe p = { {0}, {0}, {0}, 0, round };
            require(parallel_run(probe, &p) && p.parts == 4, "four-way batch");
            for (unsigned int core = 0; core < PARALLEL_CORES; core++) {
                uint32_t expected = 0;
                for (uint32_t i = core; i < 4096; i += PARALLEL_CORES) expected += i ^ round;
                require(p.sum[core] == expected && p.seen[core] == core + 1,
                        "result/core identity mismatch");
                require(p.nested_rejected[core], "nested/non-owner submit must fail");
            }
        }
        test_raster();
        if (argc > 1 && !strcmp(argv[1], "--timeout")) {
            SDL_AtomicSet(&stop[3], 1);
            SDL_WaitThread(threads[3], NULL);
            expected_timeout = 1;
            uint32_t pixels[16];
            RasterFill fill = { pixels, 4, 0, 0, 4, 4, 0 };
            parallel_run(raster_fill_rows, &fill);
            require(0, "completion timeout must halt, never return/reuse context");
        }
    }
    for (unsigned int core = 1; core < PARALLEL_CORES; core++) {
        if (threads[core]) {
            SDL_AtomicSet(&stop[core], 1);
            SDL_WaitThread(threads[core], NULL);
        }
    }
    SDL_Quit();
    puts("PASS: host-thread dispatcher checks (not ARM cache/firmware validation)");
    return 0;
}
