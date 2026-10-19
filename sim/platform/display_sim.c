/* SDL2 platform backend for the Vantage-OS simulator.
 *
 * Implements the seam declared in includes/platform.h. Built only by
 * sim/CMakeLists.txt; this file is never compiled into kernel8.img.
 */

#include "../../includes/platform.h"

#include "SDL2/SDL.h"

#include <stdlib.h>
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

static int mouseDown;
static int mouseX;
static int mouseY;

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

static void pump_events(void) {
    SDL_Event e;

    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                SDL_Quit();
                exit(0);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    mouseDown = 1;
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    mouseDown = 0;
                }
                break;
            case SDL_MOUSEMOTION:
                mouseX = e.motion.x;
                mouseY = e.motion.y;
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
                    SDL_Quit();
                    exit(0);
                }
                break;
            default:
                break;
        }
    }
}

void fb_swap(void) {
    if (!texture) {
        return;
    }
    pump_events();

    SDL_UpdateTexture(texture, NULL, backBuffer,
                      (int)(canvasW * sizeof(uint32_t)));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
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
    return 1;
}

uint32_t touch_read(int* x, int* y) {
    *x = mouseX;
    *y = mouseY;
    return mouseDown ? 1u : 0u;
}