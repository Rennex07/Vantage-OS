#include "../include/terminal.h"
#include "../include/user_lib.h"

#ifdef VANTAGE_SIM
#define USER_DATA
#else
#define USER_DATA __attribute__((section(".user.bss"), aligned(16)))
#endif

static Terminal terminal USER_DATA;
static uint32_t keyboard_columns USER_DATA;
static uint32_t keyboard_top USER_DATA;
static uint32_t keyboard_shift USER_DATA;
static uint32_t keyboard_dirty USER_DATA;
static uint32_t keyboard_pressed USER_DATA;
static uint32_t keyboard_repeat_time USER_DATA;
static uint32_t keyboard_repeat_delay USER_DATA;

#define KEYBOARD_ROWS 17U
#define KEYBOARD_KEYS 43U
#define NO_KEY (~0U)

static char keyboard_character(uint32_t key) {
    char c;
    if (key < 10) return (keyboard_shift ? "!@#$%^&*()" : "1234567890")[key];
    if (key < 20) c = "qwertyuiop"[key - 10];
    else if (key < 29) c = "asdfghjkl"[key - 20];
    else if (key >= 30 && key <= 36) c = "zxcvbnm"[key - 30];
    else return key == 39 ? ',' : key == 41 ? '.' : ' ';
    if (keyboard_shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return c;
}

/* Layout units keep rendering and hit testing identical at any supported width. */
static CanvasButton keyboard_button(uint32_t key) {
    CanvasButton b = {0};
    uint32_t row, left, right, width = keyboard_columns * TEXT_CELL_WIDTH;
    const char* label = NULL;
    if (key < 20) { row = key / 10; left = (key % 10) * 100; right = left + 100; }
    else if (key < 29) { row = 2; left = 50 + (key - 20) * 100; right = left + 100; }
    else if (key < 38) {
        row = 3;
        if (key == 29) { left = 0; right = 140; label = "Shift"; }
        else if (key == 37) { left = 840; right = 1000; label = "Del"; }
        else { left = 140 + (key - 30) * 100; right = left + 100; }
    } else {
        const uint32_t edges[] = {0, 150, 250, 700, 800, 1000};
        row = 4; left = edges[key - 38]; right = edges[key - 37];
        if (key == 38) label = "Cancel";
        else if (key == 40) label = "Space";
        else if (key == 42) label = "Enter";
    }
    b.x = 8 + left * (width - 16) / 1000 + 3;
    b.width = (right * (width - 16) / 1000) - (left * (width - 16) / 1000) - 6;
    b.y = keyboard_top * TEXT_CELL_HEIGHT + 12 + row * 50;
    b.height = 42; b.radius = 7;
    b.text_scale = label ? 1 : 2;
    b.foreground = RGB(232, 238, 248);
    b.background = key == 42 ? RGB(40, 105, 205) : key == 29 || key >= 37 ? RGB(49, 61, 80) : RGB(63, 76, 96);
    if (key == 29 && keyboard_shift) b.background = RGB(40, 105, 205);
    if (key == keyboard_pressed) b.background = RGB(83, 148, 236);
    if (label) { uint32_t i = 0; while (label[i] && i < sizeof(b.label) - 1) { b.label[i] = label[i]; i++; } }
    else b.label[0] = keyboard_character(key);
    if (key == 37 || key == 40 || (key == 42 && b.width >= 80)) b.text_scale = 2;
    return b;
}

static uint32_t keyboard_hit(int x, int y) {
    uint32_t key;
    if (x < 0 || y < 0) return NO_KEY;
    for (key = 0; key < KEYBOARD_KEYS; key++) {
        CanvasButton b = keyboard_button(key);
        if ((uint32_t)x >= b.x && (uint32_t)x - b.x < b.width &&
            (uint32_t)y >= b.y && (uint32_t)y - b.y < b.height) return key;
    }
    return NO_KEY;
}

static void keyboard_touch(const TouchEvent* touch) {
    uint32_t key, pressed;
    KeyEvent event = {KEY_NONE, 0};
    if (!keyboard_columns) return;
    key = keyboard_hit(touch->x, touch->y);
    pressed = keyboard_pressed;
    if (touch->type == TOUCH_EVENT_DOWN) {
        keyboard_pressed = key; keyboard_dirty = 1;
        if (key == 37) {
            event.type = KEY_BACKSPACE;
            terminal_key(&terminal, &event);
            keyboard_repeat_time = sys_time_us();
            keyboard_repeat_delay = 400000;
        }
        return;
    }
    if (touch->type == TOUCH_EVENT_MOVE) {
        if (key != pressed) { keyboard_pressed = NO_KEY; keyboard_dirty = 1; }
        return;
    }
    if (touch->type != TOUCH_EVENT_UP) return;
    keyboard_pressed = NO_KEY;
    if (pressed == NO_KEY) return;
    keyboard_dirty = 1;
    if (key != pressed) return;
    if (key == 29) {
        keyboard_shift = !keyboard_shift;
        return;
    }
    if (key == 37) return; /* Del acts on press and repeats while held. */
    if (key == 42) event.type = KEY_ENTER;
    else if (key == 38) event.type = KEY_CANCEL;
    else {
        event.type = KEY_TEXT;
        event.character = (uint32_t)keyboard_character(key);
        if (keyboard_shift) { keyboard_shift = 0; keyboard_dirty = 1; }
    }
    terminal_key(&terminal, &event);
}

static uint32_t keyboard_draw(void) {
    uint32_t key, success = 1;
    CanvasButton panel = {0};
    panel.y = keyboard_top * TEXT_CELL_HEIGHT;
    panel.width = keyboard_columns * TEXT_CELL_WIDTH;
    panel.height = KEYBOARD_ROWS * TEXT_CELL_HEIGHT;
    panel.background = RGB(24, 31, 43);
    if (!sys_canvas_button(&panel)) return 0;
    for (key = 0; key < KEYBOARD_KEYS; key++) {
        CanvasButton b = keyboard_button(key), shadow = b;
        shadow.y += 3; shadow.background = RGB(10, 15, 23); shadow.label[0] = 0;
        if (!sys_canvas_button(&shadow)) success = 0;
        if (!sys_canvas_button(&b)) success = 0;
    }
    return success;
}

static uint32_t info(void* context, ConsoleInfo* out) {
    (void)context;
    return sys_console_info(out);
}

static void output(void* context, char character) {
    (void)context;
    sys_console_write(&character, 1);
}

void terminal_app_init(void) {
    ConsoleInfo settings;
    uint32_t columns, rows;
    sys_canvas_set_color(RGB(16, 20, 28));
    sys_canvas_clear();
    if (!sys_console_info(&settings)) return;
    columns = settings.columns < TEXT_MAX_COLUMNS ? settings.columns : TEXT_MAX_COLUMNS;
    rows = settings.rows < TERMINAL_MAX_ROWS ? settings.rows : TERMINAL_MAX_ROWS;
    keyboard_columns = 0;
    keyboard_shift = keyboard_dirty = 0;
    keyboard_pressed = NO_KEY;
    if (columns >= 50 && rows >= KEYBOARD_ROWS + 4) {
        keyboard_columns = columns;
        keyboard_top = rows - KEYBOARD_ROWS;
        keyboard_dirty = 1;
        rows = keyboard_top;
    }
    terminal_init(&terminal, columns, rows, info, output, NULL);
}

void terminal_app_step(void) {
    KeyEvent key;
    TouchEvent touch;
    uint32_t count, row, col, changed = 0;
    for (count = 0; count < 64 && sys_key_read(&key); count++) terminal_key(&terminal, &key);
    for (count = 0; count < 64 && sys_touch_read(&touch); count++) {
        terminal_touch(&terminal, &touch);
        keyboard_touch(&touch);
    }
    if (keyboard_pressed == 37) {
        uint32_t now = sys_time_us();
        if (now - keyboard_repeat_time >= keyboard_repeat_delay) {
            const KeyEvent backspace = {KEY_BACKSPACE, 0};
            terminal_key(&terminal, &backspace);
            /* Drop overdue repeats instead of deleting a burst after a stall. */
            keyboard_repeat_time = now;
            keyboard_repeat_delay = 70000;
        }
    }
    for (row = 0; row < terminal.rows; row++) {
        TextRow text;
        if (!terminal.dirty[row]) continue;
        text.row = row;
        text.length = terminal.columns;
        text.foreground = RGB(220, 230, 238);
        text.background = RGB(16, 20, 28);
        if (terminal.editing) {
            if (terminal_editor_selected(&terminal, row)) text.background = RGB(35, 63, 91);
            else if (row >= terminal.rows - 6) text.background = RGB(49, 61, 80);
        }
        text.cursor = !terminal.editing && row == terminal.row && terminal.column < terminal.columns ? terminal.column : TEXT_NO_CURSOR;
        for (col = 0; col < TEXT_MAX_COLUMNS; col++) text.text[col] = col < terminal.columns ? terminal.cells[row][col] : ' ';
        if (sys_canvas_text_row(&text)) {
            terminal.dirty[row] = 0;
            changed = 1;
        }
    }
    if (keyboard_dirty && keyboard_draw()) { keyboard_dirty = 0; changed = 1; }
    if (changed) sys_canvas_present();
}

void user_task(void) {
    terminal_app_init();
    for (;;) {
        terminal_app_step();
        sys_frame_wait();
    }
}
