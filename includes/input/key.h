#pragma once
#include "../std.h"

enum {
    KEY_NONE,
    KEY_TEXT,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_CLEAR,
    KEY_CANCEL
};

typedef struct {
    uint32_t type;
    uint32_t character; /* Printable ASCII for KEY_TEXT. */
} KeyEvent;

uint32_t key_read(KeyEvent* event);
