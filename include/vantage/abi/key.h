#pragma once
#include <vantage/base.h>

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
