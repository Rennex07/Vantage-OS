#pragma once
#include <vantage/abi/text.h>

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

struct circle_args {
    int x;
    int y;
    int r;
};

/* Bounded pixel-coordinate UI primitive; label is inline, not a user pointer. */
typedef struct {
    uint32_t x, y, width, height, radius;
    uint32_t foreground, background;
    uint32_t text_scale; /* 0/1: native 8x16, 2: double size. */
    char label[8];
} CanvasButton;
