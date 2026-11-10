#include <vantage/kernel/window_manager.h>

#define EDGE_ZONE 100
#define MIN_HEIGHT 200
#define DRAG_IDLE 0
#define DRAG_IGNORE 1
#define DRAG_ACTIVE 2

static uint32_t touchStatus;

void env_Init(uint32_t width, uint32_t height) {
    canvas_init(width, height);
    touchStatus = touch_init();
}

void env_DrawWindow(Window* window) {
    canvas_set_color(window->bgColor);
    canvas_fill_rect(window->x, window->y, window->w, window->h);
}

void env_ListenTouch(Window* window, const TouchEvent* event) {
    int tx = event->x;
    int ty = event->y;

    if (event->type == TOUCH_EVENT_UP) {
        if (window->dragState == DRAG_ACTIVE) {
            window->x = window->fullX;
            window->y = window->fullY;
            window->w = window->fullW;
            window->h = window->fullH;
        }
        window->dragState = DRAG_IDLE;
        return;
    }

    if (event->type == TOUCH_EVENT_DOWN && window->dragState == DRAG_IDLE) {
        int inside = tx >= window->x && tx < window->x + window->w;
        int inZone = ty >= window->y + window->h - EDGE_ZONE && ty < window->y + window->h;

        if (inside && inZone) {
            window->dragState = DRAG_ACTIVE;
            window->fullX = window->x;
            window->fullY = window->y;
            window->fullW = window->w;
            window->fullH = window->h;
            window->startTouchY = ty;
            window->grabX = (tx - window->x) * 1000 / window->w;
            window->grabY = (ty - window->y) * 1000 / window->h;
        } else {
            window->dragState = DRAG_IGNORE;
        }
        return;
    }

    if (event->type == TOUCH_EVENT_MOVE && window->dragState == DRAG_ACTIVE) {
        int distance = window->startTouchY - ty;
        int maxDistance = window->fullH - MIN_HEIGHT;

        if (distance < 0) {
            distance = 0;
        }
        if (distance > maxDistance) {
            distance = maxDistance;
        }

        window->h = window->fullH - distance;
        window->w = window->fullW - distance / 2;
        window->x = tx - window->grabX * window->w / 1000;
        window->y = ty - window->grabY * window->h / 1000;
    }
}
