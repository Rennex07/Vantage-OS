#pragma once
#include <vantage/base.h>

typedef enum {
    TOUCH_EVENT_NONE = 0,
    TOUCH_EVENT_DOWN,
    TOUCH_EVENT_MOVE,
    TOUCH_EVENT_UP
} TouchEventType;

typedef struct {
    TouchEventType type;
    int x;
    int y;
} TouchEvent;
