#pragma once

#include <vantage/kernel/parallel.h>

/* Physical rows, after the display backend has applied rotation/clipping.
 * Each lane owns disjoint rows. Pitches are in pixels, not bytes. */
typedef struct {
    uint32_t* pixels;
    uint32_t pitch, x, y, width, height, color;
} RasterFill;

typedef struct {
    const uint32_t* source;
    volatile uint32_t* destination;
    uint32_t source_pitch, destination_pitch, width, height;
} RasterCopy;

void raster_fill_rows(unsigned int part, unsigned int parts, void* context);
void raster_copy_rows(unsigned int part, unsigned int parts, void* context);
