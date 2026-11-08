#pragma once
#include "../hardware.h"

#define V3D_TABLE_BYTES (4U * 1024U * 1024U)
#define V3D_ARENA_BYTES (4U * 1024U * 1024U)
#define V3D_GPU_BASE 0x01000000U

typedef struct V3d {
    const HwOps* io;
    uint32_t* table;
    unsigned char* arena;
    uint32_t width, height, pitch, bytes;
    int state, error;
} V3d;

/* Explicit exclusive controller ownership; kernel-only, core 0, masked EL1.
 * Both allocations are private contiguous page runs, retained until reboot.
 * The table must be 64 KiB aligned, arena 4 KiB aligned. */
int v3d_init(V3d* gpu, const HwOps* io, void* table, void* arena,
             uint32_t width, uint32_t height);
int v3d_clear(V3d* gpu, unsigned int surface, uint32_t rgba);
int v3d_copy(V3d* gpu, unsigned int destination, unsigned int source);
uint32_t* v3d_pixels(V3d* gpu, unsigned int surface);
int v3d_self_test(V3d* gpu);
