#pragma once

#include <vantage/base.h>
#include <vantage/abi/touch.h>

uint32_t* fb_init(uint32_t width, uint32_t height);

void put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

void fb_wait_frame(void);
void fb_swap(void);

uint32_t time_us(void);

#ifdef VANTAGE_SIM
/* Host backends have one address space: bounds for a syscall buffer. The Pi
 * resolves buffers through the calling process; see kernel/user_access.h. */
uint32_t user_buffer_valid(uintptr_t buffer, size_t size);
/* Host backend accounting; not part of the userspace ABI. */
uint64_t sim_backbuffer_bytes(void);
#endif
