#include "../../includes/env/windowManager.h"
#include "../../user/include/terminal.h"
#include "../../user/include/user_lib.h"
#include "../platform/display_sim.h"
#include "../../drivers/video/font8x16.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s (%s)\n", __LINE__, #c, SDL_GetError()); exit(1); } } while (0)
#define FG RGB(220, 230, 238)
#define BG RGB(16, 20, 28)

static void text(const char* input) {
    while (*input) {
        SDL_Event event;
        size_t count = strlen(input);
        if (count > sizeof(event.text.text) - 1) count = sizeof(event.text.text) - 1;
        SDL_zero(event);
        event.type = SDL_TEXTINPUT;
        memcpy(event.text.text, input, count);
        CHECK(SDL_PushEvent(&event) == 1);
        input += count;
    }
}

static void key(SDL_Keycode code, SDL_Keymod mod) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_KEYDOWN;
    event.key.keysym.sym = code;
    event.key.keysym.mod = (Uint16)mod;
    CHECK(SDL_PushEvent(&event) == 1);
}

static void tap(int column, int row) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 8 + (column * 100 + 50 + (row == 2 ? 50 : 0)) * 464 / 1000;
    event.button.y = 33 * 16 + 12 + row * 50 + 21;
    if (row == 2 && column == 9) { event.button.x = 435; event.button.y = 711; }
    if (row == 3 && column == 0) event.button.x = 40;
    if (row == 3 && column == 1) event.button.x = 96;
    if (row == 3 && column == 9) { event.button.x = 425; event.button.y = 761; }
    CHECK(SDL_PushEvent(&event) == 1);
    event.type = SDL_MOUSEBUTTONUP;
    CHECK(SDL_PushEvent(&event) == 1);
}

static void pixel(SDL_Renderer* renderer, int x, int y, uint32_t expected) {
    SDL_Rect rect = {x, y, 1, 1};
    uint32_t value;
    CHECK(SDL_RenderReadPixels(renderer, &rect, SDL_PIXELFORMAT_XRGB8888, &value, sizeof(value)) == 0);
    if ((value & 0xffffff) != expected) {
        fprintf(stderr, "pixel (%d,%d): %06x expected %06x\n", x, y, value & 0xffffff, expected);
        exit(1);
    }
}

static void glyph(SDL_Renderer* renderer, uint32_t column, uint32_t row, char c,
                  uint32_t foreground, uint32_t background, uint32_t cursor) {
    uint32_t x, y;
    for (y = 0; y < TEXT_CELL_HEIGHT; y++) {
        for (x = 0; x < TEXT_CELL_WIDTH; x++) {
            uint32_t ink = (font8x16[(unsigned char)c - 32][y] >> x) & 1U;
            if (cursor && y >= TEXT_CELL_HEIGHT - 2) ink = 1;
            pixel(renderer, (int)(column * TEXT_CELL_WIDTH + x),
                  (int)(row * TEXT_CELL_HEIGHT + y), ink ? foreground : background);
        }
    }
}

int main(int argc, char** argv) {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Event event;
    KeyEvent input;
    TouchEvent touch;
    ConsoleInfo info;
    TextRow row = {0};
    uint32_t i;
    env_Init(480, 800);
    sim_set_coordinate_overlay(0);
    window = SDL_GetWindowFromID(1);
    renderer = window ? SDL_GetRenderer(window) : NULL;
    CHECK(renderer);
    CHECK(sys_console_info(&info) && info.columns == 60 && info.rows == 50 &&
          info.memory_kind == CONSOLE_MEMORY_HOST && info.host_backbuffer_bytes == 1536000);
    terminal_app_init();
    terminal_app_step();
    glyph(renderer, 0, 0, 'V', FG, BG, 0); /* Overlay must not cover text. */
    pixel(renderer, 0, 550, RGB(24, 31, 43)); /* Panel and rounded button corner. */
    pixel(renderer, 11, 540, RGB(24, 31, 43));
    pixel(renderer, 15, 548, RGB(63, 76, 96));
    pixel(renderer, 54, 550, RGB(24, 31, 43)); /* Actual gap, not text separators. */
    /* The number key label occupies a centered 16x32 bitmap, not 8x16. */
    for (i = 0; i < 16 * 32; i++) {
        uint32_t x = i % 16, y = i / 16;
        uint32_t ink = (font8x16['1' - 32][y / 2] >> (x / 2)) & 1U;
        pixel(renderer, 23 + (int)x, 545 + (int)y,
              ink ? RGB(232, 238, 248) : RGB(63, 76, 96));
    }

    SDL_zero(event); event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT; event.button.x = 30; event.button.y = 610;
    CHECK(SDL_PushEvent(&event) == 1); terminal_app_step();
    pixel(renderer, 15, 598, RGB(83, 148, 236)); /* Pressed key face. */
    glyph(renderer, 2, 3, ' ', FG, BG, 1); /* Commit on release, not press. */
    SDL_zero(event); event.type = SDL_MOUSEMOTION;
    event.motion.x = 0; event.motion.y = 500;
    CHECK(SDL_PushEvent(&event) == 1); terminal_app_step();
    pixel(renderer, 15, 598, RGB(63, 76, 96));
    SDL_zero(event); event.type = SDL_MOUSEBUTTONUP; event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 30; event.button.y = 610;
    CHECK(SDL_PushEvent(&event) == 1); terminal_app_step();
    glyph(renderer, 2, 3, ' ', FG, BG, 1); /* Dragging out cancels the key. */

    /* Enter help using only touchscreen events, including deletion and shift. */
    tap(5, 2); /* h */
    tap(2, 1); /* e */
    tap(8, 2); /* l */
    tap(1, 3); /* z, deleted */
    tap(9, 2); /* DEL */
    tap(9, 1); /* p */
    tap(9, 3); /* ENTER */
    terminal_app_step();
    glyph(renderer, 0, 4, 'h', FG, BG, 0);
    tap(0, 3); tap(0, 1); /* Shift + Q */
    terminal_app_step();
    glyph(renderer, 2, 13, 'Q', FG, BG, 0); /* File commands add four help rows. */
    key(SDLK_c, KMOD_CTRL); text("clear"); key(SDLK_RETURN, KMOD_NONE); terminal_app_step();

    text("helx"); key(SDLK_BACKSPACE, KMOD_NONE); text("p"); key(SDLK_RETURN, KMOD_NONE);
    terminal_app_step();
    glyph(renderer, 0, 1, 'h', FG, BG, 0);
    text("clear"); key(SDLK_RETURN, KMOD_NONE); terminal_app_step();
    pixel(renderer, 0, 4 * 16, BG);
    text("mem"); key(SDLK_RETURN, KMOD_NONE); terminal_app_step();
    glyph(renderer, 0, 1, 'S', FG, BG, 0); /* Host statistics response. */

    /* Text events carry characters; ordinary keydown must not duplicate them. */
    key(SDLK_a, KMOD_NONE); text("a");
    CHECK(sys_key_read(&input) && input.type == KEY_TEXT && input.character == 'a');
    CHECK(!sys_key_read(&input));
    key(SDLK_l, KMOD_CTRL); CHECK(sys_key_read(&input) && input.type == KEY_CLEAR);
    key(SDLK_c, KMOD_CTRL); CHECK(sys_key_read(&input) && input.type == KEY_CANCEL);
    key(SDLK_LEFT, KMOD_NONE); text("\xc3\xa9"); CHECK(!sys_key_read(&input));

    /* Reading one input stream must not consume the other stream's events. */
    SDL_zero(event); event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT; event.button.x = 100; event.button.y = 200;
    CHECK(SDL_PushEvent(&event) == 1);
    text("z");
    event.type = SDL_MOUSEBUTTONUP; CHECK(SDL_PushEvent(&event) == 1);
    CHECK(sys_key_read(&input) && input.character == 'z');
    CHECK(sys_touch_read(&touch) && touch.type == TOUCH_EVENT_DOWN && touch.x == 100);
    CHECK(sys_touch_read(&touch) && touch.type == TOUCH_EVENT_UP);
    text("q"); CHECK(!sys_touch_read(&touch));
    CHECK(sys_key_read(&input) && input.character == 'q');

    /* Overflow must cancel a line, never turn its retained prefix into a command. */
    for (i = 0; i < 6; i++) text("abcdefghijklmnopqrstuvwxyz01234");
    key(SDLK_RETURN, KMOD_NONE);
    CHECK(sys_key_read(&input) && input.type == KEY_CANCEL);
    CHECK(!sys_key_read(&input));

    row.row = 10; row.length = 2; row.foreground = RGB(255, 0, 0);
    row.background = RGB(0, 0, 255); row.cursor = TEXT_NO_CURSOR;
    row.text[0] = 'A'; row.text[1] = ' ';
    CHECK(sys_canvas_text_row(&row)); sys_canvas_present();
    glyph(renderer, 0, 10, 'A', row.foreground, row.background, 0);
    glyph(renderer, 1, 10, ' ', row.foreground, row.background, 0);
    row.cursor = 1; CHECK(sys_canvas_text_row(&row)); sys_canvas_present();
    glyph(renderer, 1, 10, ' ', row.foreground, row.background, 1);
    /* Verify all printable glyphs, particularly baseline and descender rows. */
    row.length = 1; row.cursor = TEXT_NO_CURSOR;
    for (i = 32; i <= 126; i++) {
        row.text[0] = (char)i;
        CHECK(sys_canvas_text_row(&row)); sys_canvas_present();
        glyph(renderer, 0, 10, (char)i, row.foreground, row.background, 0);
    }
    row.row = ~0U; CHECK(!sys_canvas_text_row(&row));
    row.row = 10; row.length = TEXT_MAX_COLUMNS + 1; CHECK(!sys_canvas_text_row(&row));
    row.length = 2; row.cursor = 2; CHECK(!sys_canvas_text_row(&row));
    {
        CanvasButton button = {0};
        button.x = ~0U; button.width = button.height = 40;
        CHECK(!sys_canvas_button(&button));
        button.x = 470; CHECK(!sys_canvas_button(&button));
        button.x = 0; button.radius = 17; CHECK(!sys_canvas_button(&button));
        button.radius = 7; memset(button.label, 'x', sizeof(button.label));
        CHECK(!sys_canvas_button(&button));
        button.label[7] = 0; CHECK(!sys_canvas_button(&button)); /* Too wide. */
        button.width = 0; CHECK(!sys_canvas_button(&button));
        memset(&button, 0, sizeof(button));
        button.width = 40; button.height = 40; button.label[0] = 'a';
        button.text_scale = 3; CHECK(!sys_canvas_button(&button));
        button.text_scale = 2; button.height = 31; CHECK(!sys_canvas_button(&button));
        button.height = 40; button.width = 15; CHECK(!sys_canvas_button(&button));
    }

    if (argc == 2) {
        SDL_Surface* frame;
        text("clear"); key(SDLK_RETURN, KMOD_NONE); terminal_app_step();
        text("help"); key(SDLK_RETURN, KMOD_NONE); terminal_app_step();
        text("mem"); key(SDLK_RETURN, KMOD_NONE); terminal_app_step();
        frame = SDL_CreateRGBSurfaceWithFormat(0, 480, 800, 32, SDL_PIXELFORMAT_XRGB8888);
        CHECK(frame);
        CHECK(SDL_RenderReadPixels(renderer, NULL, frame->format->format, frame->pixels, frame->pitch) == 0);
        CHECK(SDL_SaveBMP(frame, argv[1]) == 0);
        SDL_FreeSurface(frame);
    }
    SDL_Quit();
    puts("PASS: SDL keyboard through syscalls to shell pixels, font/cursor bounds and independent input queues");
    return 0;
}
