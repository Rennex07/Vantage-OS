#include <vantage/drivers/touch.h>
#include <vantage/platform/pi4/i2c.h>
#include <vantage/platform/pi4/io.h>
#include <stdio.h>
#include <string.h>

static uint8_t sample[9];
static int read_ok = 1;

void i2c_init(void) {}
uint32_t i2c_probe(uint8_t addr) { return addr == TOUCH_ADDR; }
void uart_writeText(const char* text) { (void)text; }
uint32_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t* data, uint32_t len) {
    if (!read_ok || addr != TOUCH_ADDR || reg != 0 || len != sizeof(sample)) {
        return 0;
    }
    memcpy(data, sample, sizeof(sample));
    return 1;
}

static void set_sample(int count, int controller_event, int raw_x, int raw_y) {
    memset(sample, 0, sizeof(sample));
    sample[2] = (uint8_t)count;
    sample[3] = (uint8_t)((controller_event << 6) | ((raw_x >> 8) & 15));
    sample[4] = (uint8_t)raw_x;
    sample[5] = (uint8_t)((raw_y >> 8) & 15);
    sample[6] = (uint8_t)raw_y;
}

static int expect(TouchEventType type, int x, int y) {
    TouchEvent event;
    uint32_t present = touch_read(&event);
    if (present != (uint32_t)(type != TOUCH_EVENT_NONE) ||
        event.type != type || event.x != x || event.y != y) {
        fprintf(stderr, "got %d (%d,%d), expected %d (%d,%d)\n",
                event.type, event.x, event.y, type, x, y);
        return 0;
    }
    return 1;
}

int main(void) {
    if (!touch_init() || !expect(TOUCH_EVENT_NONE, 0, 0)) return 1;

    set_sample(1, 0, 100, 200);
    if (!expect(TOUCH_EVENT_DOWN, 200, 699) ||
        !expect(TOUCH_EVENT_NONE, 200, 699)) return 1;
    set_sample(1, 2, 101, 220);
    if (!expect(TOUCH_EVENT_MOVE, 220, 698)) return 1;

    read_ok = 0;
    if (!expect(TOUCH_EVENT_NONE, 220, 698)) return 1;
    read_ok = 1;
    set_sample(11, 0, 0, 0);
    if (!expect(TOUCH_EVENT_NONE, 220, 698)) return 1;
    set_sample(1, 3, 0, 0);
    if (!expect(TOUCH_EVENT_NONE, 220, 698)) return 1;
    set_sample(0, 0, 0, 0);
    if (!expect(TOUCH_EVENT_UP, 220, 698) ||
        !expect(TOUCH_EVENT_NONE, 220, 698)) return 1;

    set_sample(1, 2, 0, 0);
    if (!expect(TOUCH_EVENT_DOWN, 0, 799)) return 1;
    set_sample(1, 1, 0, 0);
    if (!expect(TOUCH_EVENT_UP, 0, 799)) return 1;
    return 0;
}
