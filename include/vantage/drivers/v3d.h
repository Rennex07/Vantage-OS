#pragma once
#include <vantage/drivers/hardware.h>

/* One 32-bit entry per 4 KiB page of the 4 GiB GPU address space. */
#define V3D_TABLE_BYTES (4U * 1024U * 1024U)
#define V3D_ARENA_BYTES (2U * 1024U * 1024U)
/* GPU virtual addresses: the arena, and a window that maps one present
 * target for the duration of that job only. */
#define V3D_GPU_BASE 0x01000000U
#define V3D_TARGET_BASE 0x01400000U

enum V3dState {
    V3D_QUARANTINED = -2, /* Stop unconfirmed: table and arena stay reserved until reboot. */
    V3D_STOPPED = -1,     /* Held in reset or never mapped: the caller owns both buffers again. */
    V3D_UNUSED = 0,
    V3D_READY = 1,        /* Idle: the CPU owns the surface. */
    V3D_BUSY = 2          /* Bring-up or a job in flight: the device owns the arena. */
};

/* Where the last failure happened; error holds the HwResult. */
enum V3dStage {
    V3D_STAGE_NONE, V3D_STAGE_POWER, V3D_STAGE_CLOCK, V3D_STAGE_BRIDGE,
    V3D_STAGE_IDENT, V3D_STAGE_IDLE, V3D_STAGE_MMU, V3D_STAGE_CLEAR,
    V3D_STAGE_PRESENT, V3D_STAGE_VERIFY, V3D_STAGE_COUNT
};

typedef struct V3d {
    const HwOps* io;
    uint32_t* table;
    unsigned char* arena;
    uint32_t width, height, pitch, bytes;
    int state, error, stage;
    int level;       /* How far bring-up touched the hardware; selects teardown. */
    int mapped;      /* The device has been given the page table address. */
    int present;     /* Present to an external target passed the self-test. */
    uint32_t detail; /* V3D_ERR_STAT snapshot at the failure, for the log only. */
} V3d;

/* Kernel-only: core 0, masked EL1, never re-entered. Calls are synchronous;
 * no job is in flight after any of them returns.
 *
 * table (64 KiB aligned) and arena (4 KiB aligned) are private contiguous
 * page runs lent by the caller. They return to the caller when state becomes
 * V3D_STOPPED and must stay reserved while it is V3D_QUARANTINED.
 * width * height * 4 must be a whole number of 4 KiB pages.
 *
 * Any failure after hardware was touched stops the device (bridges stopped,
 * core held in reset) before returning; a context is never restarted. */
int v3d_init(V3d* gpu, const HwOps* io, void* table, void* arena,
             uint32_t width, uint32_t height);
/* Clear, present and read back before first use. target is an optional
 * present destination with the v3d_present contract; its contents are lost. */
int v3d_self_test(V3d* gpu, void* target);
/* Draw surface, pitch == width. Valid for CPU access only while V3D_READY. */
uint32_t* v3d_surface(V3d* gpu);
int v3d_clear(V3d* gpu, uint32_t rgba);
/* Copy the surface to a caller-owned width * height buffer with pitch ==
 * width. It is mapped for this job only and must not be accessed by anyone
 * else until the call returns. HW_UNSUPPORTED (device untouched) when the
 * target is not page aligned, contiguous and below the peripheral window. */
int v3d_present(V3d* gpu, void* target);
/* Orderly teardown of a ready device. Returns the resulting V3dState. */
int v3d_stop(V3d* gpu);
const char* v3d_stage_name(int stage);

struct RenderBackend;
/* Fills name, context, surface and operations for a ready device. */
void v3d_render_backend(V3d* gpu, struct RenderBackend* backend);
