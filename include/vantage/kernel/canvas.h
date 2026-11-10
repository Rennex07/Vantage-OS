#pragma once

#include <vantage/abi/graphics.h>

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

uint32_t canvas_button(const CanvasButton* button);
