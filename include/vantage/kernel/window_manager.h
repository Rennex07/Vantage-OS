#pragma once

#include <vantage/kernel/canvas.h>
#include <vantage/drivers/touch.h>

typedef struct Window Window;

struct Window {
    int x, y, w, h;
    uint32_t titleColor;
    uint32_t bgColor;
    char title[32];
    void (*onTouch)(Window* self, int tx, int ty);

    int dragState;
    int dragOffsetX;
    int dragOffsetY;

    int fullX, fullY, fullW, fullH;
    int startTouchY;
    int grabX, grabY;
};

void env_Init(uint32_t width, uint32_t height);
void env_DrawWindow(Window* window);
void env_ListenTouch(Window* window, const TouchEvent* event);
