#include <vantage/kernel/window_manager.h>
#include <SDL2/SDL.h>
#include "platform/storage_host.h"
#include <stdio.h>
#include <stdlib.h>
#include "platform/display_sim.h"

void user_task(void);

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    const char* storage = getenv("VANTAGE_STORAGE_IMAGE");
    if (storage) {
        int result = sim_storage_open(storage, 0);
        if (result < 0) { fprintf(stderr, "Storage image mount failed (%d)\n", result); return 1; }
        if (result) fprintf(stderr, "Storage recovered one valid snapshot; inspect the image\n");
    }

    env_Init(480, 800);
    sim_set_coordinate_overlay(0);
    SDL_SetWindowTitle(SDL_GetWindowFromID(1), "Vantage-OS Terminal");
    user_task();
    return 0;
}
