/* SDL2 platform backend for the Vantage-OS simulator.
 *
 * Implements the seam declared in vantage/kernel/platform.h. Built only by
 * sim/CMakeLists.txt; this file is never compiled into kernel8.img.
 */

#include <vantage/kernel/platform.h>
#include <vantage/platform/pi4/io.h>
#include <vantage/abi/console.h>
#include "display_sim.h"

#include "SDL2/SDL.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

/* Without this the process is DPI-unaware, so Windows bitmap-stretches the
 * entire window by the system scale factor: a 480x800 canvas becomes 720x1200
 * on a 150% display and overflows the screen. Marking the process per-monitor
 * aware and pairing it with SDL_WINDOW_ALLOW_HIGHDPI makes the window that many
 * real pixels regardless of the desktop scale setting. */
static void claim_dpi_awareness(void) {
#if defined(_WIN32)
    /* Windows 10 1703+. Falls back below if unavailable. */
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return;
    }
    SetProcessDPIAware();
#endif
}

static uint32_t* backBuffer;
static uint32_t canvasW;
static uint32_t canvasH;

static SDL_Window* window;
static SDL_Renderer* renderer;
static SDL_Texture* texture;
static uint32_t lastFrame;

static int mouseDown;
static int mouseX;
static int mouseY;
static uint32_t coordinateOverlay = 1;

#define INPUT_QUEUE_SIZE 128U
static KeyEvent keys[INPUT_QUEUE_SIZE];
static TouchEvent touches[INPUT_QUEUE_SIZE];
static uint32_t keyHead, keyCount, touchHead, touchCount, discardKeyLine;

void sim_set_coordinate_overlay(uint32_t enabled) { coordinateOverlay = enabled != 0; }

uint64_t sim_backbuffer_bytes(void) { return (uint64_t)canvasW * canvasH * sizeof(uint32_t); }

static const uint8_t* coordinate_glyph(char c) {
    static const uint8_t digits[10][5] = {
        { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 },
        { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 },
        { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 },
        { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 },
        { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 }
    };
    static const uint8_t x[5] = { 5, 5, 2, 5, 5 };
    static const uint8_t y[5] = { 5, 5, 2, 2, 2 };
    static const uint8_t colon[5] = { 0, 2, 0, 2, 0 };
    static const uint8_t minus[5] = { 0, 0, 7, 0, 0 };
    static const uint8_t blank[5] = { 0, 0, 0, 0, 0 };

    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c == 'X') return x;
    if (c == 'Y') return y;
    if (c == ':') return colon;
    if (c == '-') return minus;
    return blank;
}

static void draw_coordinates(void) {
    char text[64];
    SDL_Rect background;
    size_t i;
    int row, col;

    snprintf(text, sizeof(text), "X:%d Y:%d", mouseX, mouseY);
    background.x = 6;
    background.y = 6;
    background.w = (int)strlen(text) * 12 + 12;
    background.h = 27;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer, &background);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    for (i = 0; text[i] != '\0'; i++) {
        const uint8_t* glyph = coordinate_glyph(text[i]);
        for (row = 0; row < 5; row++) {
            for (col = 0; col < 3; col++) {
                if (glyph[row] & (1 << (2 - col))) {
                    SDL_Rect pixel = { 12 + (int)i * 12 + col * 3,
                                       12 + row * 3, 3, 3 };
                    SDL_RenderFillRect(renderer, &pixel);
                }
            }
        }
    }
}

void uart_writeText(const char* buffer) {
    SDL_Log("%s", buffer);
}

uint32_t* fb_init(uint32_t width, uint32_t height) {
    canvasW = height;
    canvasH = width;

    claim_dpi_awareness();

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 0;
    }

    window = SDL_CreateWindow("Vantage-OS", SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, (int)canvasW,
                              (int)canvasH,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 0;
    }

    renderer = SDL_CreateRenderer(window, -1,
                                  SDL_RENDERER_ACCELERATED |
                                      SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return 0;
    }

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888,
                                SDL_TEXTUREACCESS_STREAMING, (int)canvasW,
                                (int)canvasH);
    if (!texture) {
        SDL_Log("SDL_CreateTexture failed: %s", SDL_GetError());
        return 0;
    }

    backBuffer = (uint32_t*)calloc((size_t)canvasW * canvasH, sizeof(uint32_t));
    keyHead = keyCount = touchHead = touchCount = discardKeyLine = 0;
    SDL_StartTextInput();
    lastFrame = time_us();
    return backBuffer;
}

void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= canvasW || y >= canvasH) {
        return;
    }
    backBuffer[y * canvasW + x] = color;
}

void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    uint32_t i;
    uint32_t j;

    if (x >= canvasW || y >= canvasH) {
        return;
    }
    if (x + w > canvasW) {
        w = canvasW - x;
    }
    if (y + h > canvasH) {
        h = canvasH - y;
    }

    for (j = 0; j < h; j++) {
        uint32_t* row = backBuffer + (size_t)(y + j) * canvasW + x;
        for (i = 0; i < w; i++) {
            row[i] = color;
        }
    }
}

static int clamp_coordinate(int value, uint32_t size) {
    if (value < 0 || size == 0) return 0;
    if ((uint32_t)value >= size) return (int)(size - 1);
    return value;
}

static void queue_key(KeyEvent key) {
    if (keyCount == INPUT_QUEUE_SIZE) {
        keyHead = keyCount = 0;
        discardKeyLine = 1;
    }
    if (discardKeyLine) {
        if (key.type != KEY_ENTER && key.type != KEY_CANCEL) return;
        discardKeyLine = 0;
        key.type = KEY_CANCEL; /* Never execute a truncated input line. */
        key.character = 0;
    }
    keys[(keyHead + keyCount++) % INPUT_QUEUE_SIZE] = key;
}

static void queue_touch(TouchEvent touch) {
    if (touchCount == INPUT_QUEUE_SIZE) {
        touchHead = touchCount = 0;
        mouseDown = 0;
        touch.type = TOUCH_EVENT_UP; /* Reset a consumer after queue overflow. */
    }
    touches[(touchHead + touchCount++) % INPUT_QUEUE_SIZE] = touch;
}

static void pump_input(void) {
    SDL_Event e;
    uint32_t count;

    SDL_PumpEvents();
    for (count = 0; count < 256 && SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0; count++) {
        TouchEvent touch = {TOUCH_EVENT_NONE, 0, 0};
        KeyEvent key = {KEY_NONE, 0};
        switch (e.type) {
            case SDL_QUIT:
                SDL_Quit();
                exit(0);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT && !mouseDown) {
                    mouseDown = 1;
                    mouseX = clamp_coordinate(e.button.x, canvasW);
                    mouseY = clamp_coordinate(e.button.y, canvasH);
                    touch.type = TOUCH_EVENT_DOWN;
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT && mouseDown) {
                    mouseDown = 0;
                    mouseX = clamp_coordinate(e.button.x, canvasW);
                    mouseY = clamp_coordinate(e.button.y, canvasH);
                    touch.type = TOUCH_EVENT_UP;
                }
                break;
            case SDL_MOUSEMOTION:
                if (mouseDown) {
                    int x = clamp_coordinate(e.motion.x, canvasW);
                    int y = clamp_coordinate(e.motion.y, canvasH);
                    if (mouseX != x || mouseY != y) {
                        mouseX = x;
                        mouseY = y;
                        touch.type = TOUCH_EVENT_MOVE;
                    }
                }
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
                    SDL_Quit();
                    exit(0);
                } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && mouseDown) {
                    mouseDown = 0;
                    touch.type = TOUCH_EVENT_UP;
                }
                break;
            case SDL_TEXTINPUT: {
                size_t i;
                for (i = 0; i < sizeof(e.text.text) && e.text.text[i]; i++) {
                    unsigned char c = (unsigned char)e.text.text[i];
                    if (c >= 32 && c <= 126) {
                        key.type = KEY_TEXT;
                        key.character = c;
                        queue_key(key);
                    }
                }
                break;
            }
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) key.type = KEY_ENTER;
                else if (e.key.keysym.sym == SDLK_BACKSPACE) key.type = KEY_BACKSPACE;
                else if ((e.key.keysym.mod & KMOD_CTRL) && e.key.keysym.sym == SDLK_l) key.type = KEY_CLEAR;
                else if ((e.key.keysym.mod & KMOD_CTRL) && e.key.keysym.sym == SDLK_c) key.type = KEY_CANCEL;
                if (key.type != KEY_NONE) queue_key(key);
                break;
            default:
                break;
        }
        if (touch.type != TOUCH_EVENT_NONE) {
            touch.x = mouseX;
            touch.y = mouseY;
            queue_touch(touch);
        }
    }
}

uint32_t key_read(KeyEvent* event) {
    if (!event) return 0;
    pump_input();
    event->type = KEY_NONE;
    event->character = 0;
    if (!keyCount) return 0;
    *event = keys[keyHead++ % INPUT_QUEUE_SIZE];
    keyCount--;
    return 1;
}

static uint32_t poll_touch_event(TouchEvent* result) {
    pump_input();
    result->type = TOUCH_EVENT_NONE;
    result->x = mouseX;
    result->y = mouseY;
    if (touchCount) {
        *result = touches[touchHead++ % INPUT_QUEUE_SIZE];
        touchCount--;
        return 1;
    }
    return 0;
}

void fb_wait_frame(void) {
    uint32_t elapsed = time_us() - lastFrame;

    if (elapsed < 16667u) {
        SDL_Delay((16667u - elapsed + 999u) / 1000u);
    }
    lastFrame = time_us();
}

void fb_swap(void) {
    if (!texture) {
        return;
    }
    SDL_UpdateTexture(texture, NULL, backBuffer,
                      (int)(canvasW * sizeof(uint32_t)));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    if (coordinateOverlay) draw_coordinates();
    SDL_RenderPresent(renderer);
}

uint32_t time_us(void) {
    static Uint64 freq = 0;

    if (freq == 0) {
        freq = SDL_GetPerformanceFrequency();
        if (freq == 0) {
            return 0;
        }
    }
    return (uint32_t)((SDL_GetPerformanceCounter() * 1000000ULL) / freq);
}

uint32_t touch_init(void) {
    mouseDown = 0;
    mouseX = 0;
    mouseY = 0;
    touchHead = touchCount = 0;
    return 1;
}

uint32_t touch_read(TouchEvent* event) {
    return poll_touch_event(event);
}

uint32_t user_buffer_valid(uintptr_t buffer, size_t size) {
    /* The desktop process owns its pointers; reject null and wrapped buffers. */
    return buffer != 0 && size <= (uintptr_t)-1 - buffer;
}
