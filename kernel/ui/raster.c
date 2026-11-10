#include <vantage/kernel/raster.h>

void raster_fill_rows(unsigned int part, unsigned int parts, void* context) {
    const RasterFill* fill = context;
    uint32_t first = fill->height * part / parts;
    uint32_t end = fill->height * (part + 1) / parts;
    for (uint32_t y = first; y < end; y++) {
        uint32_t* row = fill->pixels + (fill->y + y) * fill->pitch + fill->x;
        for (uint32_t x = 0; x < fill->width; x++) row[x] = fill->color;
    }
}

void raster_copy_rows(unsigned int part, unsigned int parts, void* context) {
    const RasterCopy* copy = context;
    uint32_t first = copy->height * part / parts;
    uint32_t end = copy->height * (part + 1) / parts;
    for (uint32_t y = first; y < end; y++) {
        const uint32_t* source = copy->source + y * copy->source_pitch;
        volatile uint32_t* destination = copy->destination + y * copy->destination_pitch;
        for (uint32_t x = 0; x < copy->width; x++) destination[x] = source[x];
    }
}
