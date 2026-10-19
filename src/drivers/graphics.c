#include "../../includes/graphics.h"
#include "../../includes/platform.h"

static uint32_t canvasWidth;
static uint32_t canvasHeight;
static uint32_t canvasColor = 0x00FFFFFF;

static void plot(int x, int y) {
    if (x < 0 || y < 0 || x >= (int)canvasWidth || y >= (int)canvasHeight) {
        return;
    }
    put_pixel(x, y, canvasColor);
}

static void hline(int x0, int x1, int y) {
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y < 0 || y >= (int)canvasHeight || x1 < 0 || x0 >= (int)canvasWidth) {
        return;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (x1 >= (int)canvasWidth) {
        x1 = canvasWidth - 1;
    }
    fill_rect(x0, y, x1 - x0 + 1, 1, canvasColor);
}

void canvas_init(uint32_t width, uint32_t height) {
    canvasWidth = width;
    canvasHeight = height;
    fb_init(800, 480);
}

void canvas_set_color(uint32_t color) {
    canvasColor = color;
}

void canvas_clear(void) {
    fill_rect(0, 0, canvasWidth, canvasHeight, canvasColor);
}

void canvas_line(int x0, int y0, int x1, int y1) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        plot(x0, y0);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void canvas_rect(int x, int y, int w, int h) {
    canvas_line(x, y, x + w - 1, y);
    canvas_line(x, y + h - 1, x + w - 1, y + h - 1);
    canvas_line(x, y, x, y + h - 1);
    canvas_line(x + w - 1, y, x + w - 1, y + h - 1);
}

void canvas_fill_rect(int x, int y, int w, int h) {
    int x1 = x + w;
    int y1 = y + h;

    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x1 > (int)canvasWidth) {
        x1 = canvasWidth;
    }
    if (y1 > (int)canvasHeight) {
        y1 = canvasHeight;
    }
    if (x1 <= x || y1 <= y) {
        return;
    }
    fill_rect(x, y, x1 - x, y1 - y, canvasColor);
}

void canvas_circle(int cx, int cy, int r) {
    int x = r;
    int y = 0;
    int err = 1 - r;

    while (x >= y) {
        plot(cx + x, cy + y);
        plot(cx - x, cy + y);
        plot(cx + x, cy - y);
        plot(cx - x, cy - y);
        plot(cx + y, cy + x);
        plot(cx - y, cy + x);
        plot(cx + y, cy - x);
        plot(cx - y, cy - x);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void canvas_fill_circle(int cx, int cy, int r) {
    int x = r;
    int y = 0;
    int err = 1 - r;

    while (x >= y) {
        hline(cx - x, cx + x, cy + y);
        hline(cx - x, cx + x, cy - y);
        hline(cx - y, cx + y, cy + x);
        hline(cx - y, cx + y, cy - x);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void canvas_present(void) {
    fb_swap();
}