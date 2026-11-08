#include "../../includes/video/graphics.h"
#include "../../includes/platform.h"

#include "../../includes/io.h"
#include "font8x16.h"

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

    uart_writeText("[OK] Vantage graphics driver...\n");
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

uint32_t canvas_text_columns(void) { return canvasWidth / TEXT_CELL_WIDTH; }
uint32_t canvas_text_rows(void) { return canvasHeight / TEXT_CELL_HEIGHT; }

uint32_t canvas_text_row(const TextRow* row) {
    uint32_t i, y, bit, old_color = canvasColor;
    uint32_t top;
    if (!row || row->row >= canvas_text_rows() || row->length > TEXT_MAX_COLUMNS ||
        row->length > canvas_text_columns() ||
        (row->cursor != TEXT_NO_CURSOR && row->cursor >= row->length)) return 0;
    top = row->row * TEXT_CELL_HEIGHT;
    canvas_set_color(row->background);
    canvas_fill_rect(0, (int)top, (int)(row->length * TEXT_CELL_WIDTH), TEXT_CELL_HEIGHT);
    canvas_set_color(row->foreground);
    for (i = 0; i < row->length; i++) {
        uint32_t c = (uint8_t)row->text[i];
        if (c < 32 || c > 126) c = '?';
        for (y = 0; y < TEXT_CELL_HEIGHT; y++) {
            uint32_t bits = font8x16[c - 32][y];
            for (bit = 0; bit < 8;) {
                uint32_t start = bit;
                if (!(bits & (1U << bit))) { bit++; continue; }
                while (bit < 8 && (bits & (1U << bit))) bit++;
                canvas_fill_rect((int)(i * TEXT_CELL_WIDTH + start), (int)(top + y),
                                 (int)(bit - start), 1);
            }
        }
    }
    if (row->cursor != TEXT_NO_CURSOR)
        canvas_fill_rect((int)(row->cursor * TEXT_CELL_WIDTH), (int)(top + TEXT_CELL_HEIGHT - 2),
                         TEXT_CELL_WIDTH, 2);
    canvasColor = old_color;
    return 1;
}

uint32_t canvas_button(const CanvasButton* b) {
    uint32_t length = 0, y, x, i, old_color = canvasColor;
    uint32_t scale;
    if (!b || !b->width || !b->height || b->x >= canvasWidth || b->y >= canvasHeight ||
        b->width > canvasWidth - b->x || b->height > canvasHeight - b->y ||
        b->radius > 16 || b->radius > b->width / 2 || b->radius > b->height / 2 ||
        b->text_scale > 2) return 0;
    scale = b->text_scale ? b->text_scale : 1;
    while (length < sizeof(b->label) && b->label[length]) length++;
    if (length == sizeof(b->label) || length * TEXT_CELL_WIDTH * scale > b->width ||
        (length && b->height < TEXT_CELL_HEIGHT * scale)) return 0;
    canvas_set_color(b->background);
    for (y = 0; y < b->height; y++) {
        uint32_t inset = 0;
        uint32_t edge = y < b->height / 2 ? y : b->height - 1 - y;
        if (edge < b->radius) {
            uint32_t dy = b->radius - 1 - edge;
            while (inset < b->radius) {
                uint32_t dx = b->radius - 1 - inset;
                if (dx * dx + dy * dy <= (b->radius - 1) * (b->radius - 1)) break;
                inset++;
            }
        }
        canvas_fill_rect((int)(b->x + inset), (int)(b->y + y), (int)(b->width - 2 * inset), 1);
    }
    canvas_set_color(b->foreground);
    for (i = 0; i < length; i++) {
        uint32_t c = (uint8_t)b->label[i];
        if (c < 32 || c > 126) c = '?';
        for (y = 0; y < TEXT_CELL_HEIGHT; y++)
            for (x = 0; x < TEXT_CELL_WIDTH; x++)
                if (font8x16[c - 32][y] & (1U << x))
                    canvas_fill_rect((int)(b->x + (b->width - length * TEXT_CELL_WIDTH * scale) / 2 +
                                          (i * TEXT_CELL_WIDTH + x) * scale),
                                     (int)(b->y + (b->height - TEXT_CELL_HEIGHT * scale) / 2 + y * scale),
                                     (int)scale, (int)scale);
    }
    canvasColor = old_color;
    return 1;
}
