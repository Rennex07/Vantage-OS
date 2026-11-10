#pragma once

#include <vantage/kernel/raster.h>

/* Backend seam between the display layer and its renderers. CPU raster is
 * always available. An accelerated backend owns one draw surface and takes
 * the operations it supports; everything else is CPU raster on that surface.
 *
 * Core 0 only. An operation runs from entry to return without being preempted
 * or re-entered: a backend may have a device job in flight or raster workers
 * writing the surface. Re-entry is a contract violation and halts. */
enum RenderResult {
    RENDER_FAILED = -1, /* Device stopped; pixels the operation targets are undefined. */
    RENDER_SKIP = 0,    /* Unsupported here; nothing was touched. */
    RENDER_DONE = 1
};

typedef struct RenderBackend {
    const char* name;
    void* context;
    /* width * height pixels, pitch == width. The CPU may access it only
     * between backend operations. */
    uint32_t* surface;
    int (*fill)(void* context, const RasterFill* fill);
    int (*present)(void* context, const RasterCopy* copy);
    /* Called once after RENDER_FAILED, when the renderer has copied the frame
     * out and no longer references the surface. */
    void (*retired)(void* context);
} RenderBackend;

typedef struct Renderer {
    uint32_t* pixels;   /* Active draw surface. */
    uint32_t* fallback; /* CPU-owned surface, retained for the renderer's lifetime. */
    uint32_t width, height;
    const RenderBackend* backend; /* NULL selects CPU rendering. */
    uint32_t busy;
} Renderer;

/* pixels is a caller-owned width * height surface that outlives the renderer. */
void render_init(Renderer* renderer, uint32_t* pixels, uint32_t width, uint32_t height);
/* Moves the current frame to the backend surface. The backend object must
 * outlive the renderer. Returns zero without changes when already attached. */
uint32_t render_attach(Renderer* renderer, const RenderBackend* backend);
void render_pixel(Renderer* renderer, uint32_t x, uint32_t y, uint32_t color);
void render_fill(Renderer* renderer, uint32_t x, uint32_t y,
                 uint32_t width, uint32_t height, uint32_t color);
/* Copies the frame to a destination that no other writer touches meanwhile.
 * destination_pitch is in pixels. */
void render_present(Renderer* renderer, volatile uint32_t* destination,
                    uint32_t destination_pitch);
