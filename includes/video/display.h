#pragma once

#include "../std.h"

#define MBOX_BASE 0xFE00B880
#define MBOX_READ (MBOX_BASE + 0x00)
#define MBOX_STATUS (MBOX_BASE + 0x18)
#define MBOX_WRITE (MBOX_BASE + 0x20)
#define MBOX_FULL 0x80000000
#define MBOX_EMPTY 0x40000000
#define MBOX_RESPONSE 0x80000000
#define MBOX_CH_PROP 8
#define TIMER_CLO 0xFE003004

uint32_t time_us(void);

uint32_t mbox_call(uint8_t ch);
uint32_t* fb_init(uint32_t width, uint32_t height);
void put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_wait_frame(void);
void fb_swap(void);
struct V3d;
uint32_t fb_enable_v3d(struct V3d* gpu);
