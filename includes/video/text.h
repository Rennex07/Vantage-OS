#pragma once
#include "../std.h"

#define TEXT_CELL_WIDTH 8U
#define TEXT_CELL_HEIGHT 16U
#define TEXT_MAX_COLUMNS 96U
#define TEXT_NO_CURSOR (~0U)

/* Fixed-size syscall payload: no nested user pointers. Coordinates are cells. */
typedef struct {
    uint32_t row;
    uint32_t length;
    uint32_t foreground;
    uint32_t background;
    uint32_t cursor;
    char text[TEXT_MAX_COLUMNS];
} TextRow;
