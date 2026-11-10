#include <vantage/kernel/window_manager.h>
#include <vantage/user.h>
#include <SDL2/SDL.h>

static int check_pixel(SDL_Renderer* renderer, int x, int y, uint32_t expected) {
    SDL_Rect rect = { x, y, 1, 1 };
    uint32_t pixel = 0;

    if (SDL_RenderReadPixels(renderer, &rect, SDL_PIXELFORMAT_XRGB8888,
                             &pixel, sizeof(pixel)) != 0) {
        SDL_Log("Readback failed: %s", SDL_GetError());
        return 0;
    }
    if ((pixel & 0x00ffffffu) != expected) {
        SDL_Log("Unexpected pixel at (%d, %d): %08x", x, y, pixel);
        return 0;
    }
    return 1;
}

int main(int argc, char* argv[]) {
    SDL_Event event;
    SDL_Window* window;
    SDL_Renderer* renderer;
    TouchEvent touch;
    int width, height;
    Window test_window = { 0 };
    (void)argc;
    (void)argv;

    env_Init(480, 800);
    window = SDL_GetWindowFromID(1);
    renderer = window ? SDL_GetRenderer(window) : NULL;
    if (!renderer) {
        SDL_Log("Simulator initialization failed: %s", SDL_GetError());
        return 1;
    }
    SDL_GetWindowSize(window, &width, &height);
    if (width != 480 || height != 800 || sys_touch_read(&touch) != 0 ||
        touch.type != TOUCH_EVENT_NONE) {
        return 1;
    }

    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 120;
    event.button.y = 240;
    if (SDL_PushEvent(&event) != 1 || sys_touch_read(&touch) != 1 ||
        touch.type != TOUCH_EVENT_DOWN || touch.x != 120 || touch.y != 240) {
        SDL_Log("Mouse press did not reach the syscall dispatcher");
        return 1;
    }

    sys_canvas_set_color(RGB(10, 10, 30));
    sys_canvas_clear();
    sys_canvas_set_color(RGB(255, 255, 255));
    sys_canvas_fill_circle(touch.x, touch.y, 12);
    sys_canvas_present();
    if (!check_pixel(renderer, 0, 0, RGB(10, 10, 30)) ||
        !check_pixel(renderer, 120, 240, RGB(255, 255, 255)) ||
        !check_pixel(renderer, 140, 240, RGB(10, 10, 30)) ||
        !check_pixel(renderer, 12, 12, RGB(255, 255, 255)) ||
        !check_pixel(renderer, 36, 12, RGB(0, 0, 0)) ||
        !check_pixel(renderer, 39, 12, RGB(255, 255, 255))) {
        return 1;
    }

    SDL_zero(event);
    event.type = SDL_MOUSEMOTION;
    event.motion.x = 200;
    event.motion.y = 300;
    if (sys_touch_read(&touch) != 0 || touch.type != TOUCH_EVENT_NONE ||
        SDL_PushEvent(&event) != 1 || sys_touch_read(&touch) != 1 ||
        touch.type != TOUCH_EVENT_MOVE || touch.x != 200 || touch.y != 300) {
        SDL_Log("Move failed: type=%d at %d,%d", touch.type, touch.x, touch.y);
        return 1;
    }
    sys_canvas_present();
    if (!check_pixel(renderer, 36, 12, RGB(255, 255, 255)) ||
        !check_pixel(renderer, 108, 21, RGB(0, 0, 0))) {
        SDL_Log("Coordinate overlay did not update");
        return 1;
    }

    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONUP;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 220;
    event.button.y = 320;
    if (SDL_PushEvent(&event) != 1 || sys_touch_read(&touch) != 1 ||
        touch.type != TOUCH_EVENT_UP || touch.x != 220 || touch.y != 320 ||
        sys_touch_read(&touch) != 0 || touch.type != TOUCH_EVENT_NONE) {
        SDL_Log("Release failed: type=%d at %d,%d", touch.type, touch.x, touch.y);
        return 1;
    }

    /* Press and release within one frame must not disappear. */
    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 40;
    event.button.y = 50;
    if (SDL_PushEvent(&event) != 1) return 1;
    event.type = SDL_MOUSEBUTTONUP;
    if (SDL_PushEvent(&event) != 1) return 1;
    sys_canvas_present();
    if (!sys_touch_read(&touch) || touch.type != TOUCH_EVENT_DOWN ||
        touch.x != 40 || touch.y != 50 ||
        !sys_touch_read(&touch) || touch.type != TOUCH_EVENT_UP ||
        touch.x != 40 || touch.y != 50 ||
        sys_touch_read(&touch) || touch.type != TOUCH_EVENT_NONE) {
        SDL_Log("Quick click failed: type=%d at %d,%d", touch.type, touch.x, touch.y);
        return 1;
    }
    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 60;
    event.button.y = 70;
    if (SDL_PushEvent(&event) != 1) return 1;
    SDL_zero(event);
    event.type = SDL_WINDOWEVENT;
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    if (SDL_PushEvent(&event) != 1 ||
        !sys_touch_read(&touch) || touch.type != TOUCH_EVENT_DOWN ||
        !sys_touch_read(&touch) || touch.type != TOUCH_EVENT_UP ||
        touch.x != 60 || touch.y != 70) return 1;

    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = -10;
    event.button.y = -20;
    if (SDL_PushEvent(&event) != 1 || !sys_touch_read(&touch) ||
        touch.type != TOUCH_EVENT_DOWN || touch.x != 0 || touch.y != 0) return 1;
    SDL_zero(event);
    event.type = SDL_MOUSEMOTION;
    event.motion.x = width + 120;
    event.motion.y = height + 100;
    if (SDL_PushEvent(&event) != 1 || !sys_touch_read(&touch) ||
        touch.type != TOUCH_EVENT_MOVE || touch.x != width - 1 ||
        touch.y != height - 1) return 1;
    event.motion.x = width + 220;
    event.motion.y = height + 200;
    if (SDL_PushEvent(&event) != 1 || sys_touch_read(&touch) ||
        touch.type != TOUCH_EVENT_NONE || touch.x != width - 1 ||
        touch.y != height - 1) return 1;
    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONUP;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = -50;
    event.button.y = height + 100;
    if (SDL_PushEvent(&event) != 1 || !sys_touch_read(&touch) ||
        touch.type != TOUCH_EVENT_UP || touch.x != 0 ||
        touch.y != height - 1) return 1;
    sys_canvas_set_color(RGB(10, 10, 30));
    sys_canvas_clear();
    sys_canvas_present();
    if (!check_pixel(renderer, 120, 240, RGB(10, 10, 30))) {
        return 1;
    }

    test_window.w = 480;
    test_window.h = 800;
    touch.type = TOUCH_EVENT_DOWN;
    touch.x = 100;
    touch.y = 750;
    env_ListenTouch(&test_window, &touch);
    touch.type = TOUCH_EVENT_MOVE;
    touch.y = 650;
    env_ListenTouch(&test_window, &touch);
    if (test_window.h != 700 || test_window.w != 430) return 1;
    touch.type = TOUCH_EVENT_NONE;
    env_ListenTouch(&test_window, &touch);
    if (test_window.h != 700) return 1;
    touch.type = TOUCH_EVENT_UP;
    env_ListenTouch(&test_window, &touch);
    if (test_window.x != 0 || test_window.y != 0 ||
        test_window.w != 480 || test_window.h != 800) return 1;

    SDL_Quit();
    return 0;
}
