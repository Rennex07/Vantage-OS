#include <vantage/kernel/window_manager.h>
#include <vantage/kernel/files.h>
#include "user/apps/terminal/terminal.h"
#include <vantage/user.h>
#include "../platform/display_sim.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage_fixture.h"

static void text(const char* input) {
    while (*input) {
        SDL_Event event;
        SDL_zero(event); event.type = SDL_TEXTINPUT;
        event.text.text[0] = *input++;
        CHECK(SDL_PushEvent(&event) == 1);
        terminal_app_step();
    }
}

static void enter(void) {
    SDL_Event event;
    SDL_zero(event); event.type = SDL_KEYDOWN; event.key.keysym.sym = SDLK_RETURN;
    CHECK(SDL_PushEvent(&event) == 1); terminal_app_step();
}

static void touch(int x, int y, int drag) {
    SDL_Event event;
    SDL_zero(event); event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT; event.button.x = x; event.button.y = y;
    CHECK(SDL_PushEvent(&event) == 1); terminal_app_step();
    if (drag) {
        SDL_zero(event); event.type = SDL_MOUSEMOTION;
        event.motion.x = 0; event.motion.y = 0;
        CHECK(SDL_PushEvent(&event) == 1); terminal_app_step();
    }
    SDL_zero(event); event.type = SDL_MOUSEBUTTONUP;
    event.button.button = SDL_BUTTON_LEFT; event.button.x = x; event.button.y = y;
    CHECK(SDL_PushEvent(&event) == 1); terminal_app_step();
}

static void button(uint32_t action, int drag) {
    static const uint32_t positions[] = {0, 1, 4, 5, 6, 7, 2, 3};
    CHECK(action < sizeof(positions) / sizeof(positions[0]));
    uint32_t position = positions[action];
    touch((int)(position % 4) * 120 + 60, (27 + (int)(position / 4) * 3) * 16 + 24, drag);
}

static void contents(const char* expected) {
    unsigned char buffer[256];
    int64_t handle = sys_file_open("/ram/notes", FILE_READ);
    CHECK(handle > 0);
    int count = sys_file_read((uint64_t)handle, buffer, sizeof(buffer));
    CHECK(count == (int)strlen(expected) && !memcmp(buffer, expected, (size_t)count));
    CHECK(sys_file_close((uint64_t)handle) == 0);
}

int main(int argc, char** argv) {
    Store store;
    fixture_reset();
    CHECK(store_format(&store, &device, &region, STORE_FORMAT_CONFIRM) == 0);
    CHECK(files_mount(&device, 0) == 0);
    env_Init(480, 800); sim_set_coordinate_overlay(0);
    terminal_app_init(); terminal_app_step();
    text("edit notes"); enter();
    text("first line"); enter(); text("second line");
    button(EDIT_SAVE, 0); contents("first line\nsecond line");
    /* Select the beginning of the first line, then type with the touch keyboard. */
    touch(6 * 8, 2 * 16, 0);
    touch(286, 661, 0); /* h */
    button(EDIT_SAVE, 0); contents("hfirst line\nsecond line");
    /* A drag out of Drop must not remove the selected line. */
    button(EDIT_DROP, 1); button(EDIT_SAVE, 0); contents("hfirst line\nsecond line");
    button(EDIT_DOWN, 0); button(EDIT_NEW, 0); text("third line");
    button(EDIT_SAVE, 0); contents("hfirst line\nsecond line\nthird line");
    button(EDIT_DROP, 0); button(EDIT_SAVE, 0); contents("hfirst line\nsecond line");
    /* The second row contains Up, Down, Left and Right in that order. */
    button(EDIT_UP, 0); button(EDIT_RIGHT, 0); button(EDIT_LEFT, 0);
    text("!"); button(EDIT_SAVE, 0); contents("hfirst line!\nsecond line");
    button(EDIT_DOWN, 0);
    text("unsaved"); button(EDIT_EXIT, 0);
    /* First Exit must leave the editor open; Save keeps the buffer. */
    button(EDIT_SAVE, 0); contents("hfirst line!\nsecond lineunsaved");
    if (argc == 2) {
        SDL_Window* window = SDL_GetWindowFromID(1);
        SDL_Renderer* renderer = window ? SDL_GetRenderer(window) : NULL;
        SDL_Surface* frame = SDL_CreateRGBSurfaceWithFormat(0, 480, 800, 32, SDL_PIXELFORMAT_XRGB8888);
        CHECK(renderer && frame);
        CHECK(SDL_RenderReadPixels(renderer, NULL, frame->format->format, frame->pixels, frame->pitch) == 0);
        CHECK(SDL_SaveBMP(frame, argv[1]) == 0);
        SDL_FreeSurface(frame);
    }
    button(EDIT_EXIT, 0);
    CHECK(files_unmount() == 0 && files_mount(&device, 0) == 0);
    contents("hfirst line!\nsecond lineunsaved");
    CHECK(files_unmount() == 0);
    SDL_Quit();
    puts("PASS: touch line selection, keyboard edits, save, drag cancellation, new/drop and exit confirmation");
    return 0;
}
