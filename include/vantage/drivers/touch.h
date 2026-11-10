#pragma once

#include <vantage/abi/touch.h>

#define TOUCH_ADDR 0x38
#define PANEL_ADDR 0x45
#define TOUCH_WIDTH 800
#define TOUCH_HEIGHT 480
#define TOUCH_FLIP_X 1
#define TOUCH_FLIP_Y 1

uint32_t touch_init(void);
/* Returns 1 when an event is available, otherwise 0 and type NONE. */
uint32_t touch_read(TouchEvent* event);
