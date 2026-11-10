#include <vantage/drivers/key.h>
#include <vantage/platform/pi4/io.h>

static uint32_t escape, previous_cr, discard;

uint32_t key_read(KeyEvent* event) {
    uint32_t attempts;
    if (!event) return 0;
    event->type = KEY_NONE;
    event->character = 0;
    for (attempts = 0; attempts < 32; attempts++) {
        uint8_t c;
        uint32_t result = uart_read_byte(&c);
        if (!result) return 0;
        if (result == 2) { discard = 1; escape = previous_cr = 0; continue; }
        if (c == '\n' && previous_cr) { previous_cr = 0; continue; }
        previous_cr = c == '\r';
        if (discard) {
            if (c != '\r' && c != '\n') continue;
            discard = 0;
            event->type = KEY_CANCEL;
        } else if (c == '\r' || c == '\n') {
            escape = 0;
            event->type = KEY_ENTER;
        } else if (escape) {
            if (escape == 1 && (c == '[' || c == 'O')) escape = 2;
            else if (escape == 1 || (c >= 0x40 && c <= 0x7e)) escape = 0;
            continue; /* Unsupported ANSI navigation sequences do not become text. */
        } else if (c == 0x1b) { escape = 1; continue; }
        else if (c == 8 || c == 127) event->type = KEY_BACKSPACE;
        else if (c == 12) event->type = KEY_CLEAR;
        else if (c == 3) event->type = KEY_CANCEL;
        else if (c >= 32 && c <= 126) {
            event->type = KEY_TEXT;
            event->character = c;
        } else continue;
        return 1;
    }
    return 0;
}
