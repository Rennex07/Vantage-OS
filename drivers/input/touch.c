#include <vantage/drivers/touch.h>
#include <vantage/platform/pi4/i2c.h>
#include <vantage/platform/pi4/io.h>

#define TOUCH_POINTS 10
#define CONTROLLER_DOWN 0
#define CONTROLLER_UP 1
#define CONTROLLER_CONTACT 2

static uint32_t lastDown;
static int lastX;
static int lastY;

uint32_t touch_init(void) {
    lastDown = 0;
    lastX = 0;
    lastY = 0;
    i2c_init();
    uart_writeText("[OK] Vantage touch driver...\n");
    return i2c_probe(TOUCH_ADDR);
}

uint32_t touch_read(TouchEvent* result) {
    uint8_t buf[9];
    int down;
    int x = lastX;
    int y = lastY;

    result->type = TOUCH_EVENT_NONE;
    result->x = lastX;
    result->y = lastY;

    if (!i2c_read_reg(TOUCH_ADDR, 0, buf, sizeof(buf))) {
        return 0;
    }

    uint8_t count = buf[2] & 0xF;
    uint8_t event = (buf[3] >> 6) & 3;

    if (count > TOUCH_POINTS) {
        return 0;
    }
    if (count == 0 || event == CONTROLLER_UP) {
        down = 0;
    } else if (event == CONTROLLER_DOWN || event == CONTROLLER_CONTACT) {
        int rawX = ((buf[3] & 0xF) << 8) | buf[4];
        int rawY = ((buf[5] & 0xF) << 8) | buf[6];
        int panelX = TOUCH_FLIP_X ? TOUCH_WIDTH - 1 - rawX : rawX;
        int panelY = TOUCH_FLIP_Y ? TOUCH_HEIGHT - 1 - rawY : rawY;

        x = TOUCH_HEIGHT - 1 - panelY;
        y = panelX;
        down = 1;
    } else {
        return 0;
    }

    if (down && !lastDown) {
        result->type = TOUCH_EVENT_DOWN;
    } else if (!down && lastDown) {
        result->type = TOUCH_EVENT_UP;
    } else if (down && (x != lastX || y != lastY)) {
        result->type = TOUCH_EVENT_MOVE;
    }

    lastDown = down;
    lastX = x;
    lastY = y;
    result->x = x;
    result->y = y;
    return result->type != TOUCH_EVENT_NONE;
}
