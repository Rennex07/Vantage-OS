#pragma once

#include "../std.h"
#include "text.h"

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

void canvas_init(uint32_t width, uint32_t height);
void canvas_set_color(uint32_t color);
void canvas_clear(void);
void canvas_line(int x0, int y0, int x1, int y1);
void canvas_rect(int x, int y, int w, int h);
void canvas_fill_rect(int x, int y, int w, int h);
void canvas_circle(int cx, int cy, int r);
void canvas_fill_circle(int cx, int cy, int r);
void canvas_present(void);

uint32_t canvas_text_columns(void);
uint32_t canvas_text_rows(void);
uint32_t canvas_text_row(const TextRow* row);

/* Bounded pixel-coordinate UI primitive; label is inline, not a user pointer. */
typedef struct {
    uint32_t x, y, width, height, radius;
    uint32_t foreground, background;
    uint32_t text_scale; /* 0/1: native 8x16, 2: double size. */
    char label[8];
} CanvasButton;

uint32_t canvas_button(const CanvasButton* button);
