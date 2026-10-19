#pragma once

#include "stdint.h"

uint32_t* fb_init(uint32_t width, uint32_t height);

void put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

void fb_swap(void);

uint32_t time_us(void);

uint32_t touch_init(void);
uint32_t touch_read(int* x, int* y);