#include "../includes/environment/windowManager.h"

static Window window;

int main(void) {
    env_Init(480, 800);

    window.w = 480;
    window.h = 800;
    window.x = 0;
    window.y = 0;
    window.bgColor = RGB(0, 0, 125);

    while (1) {
        int tx = 0;
        int ty = 0;
        uint32_t touching = touch_read(&tx, &ty);

        env_ListenTouch(&window, tx, ty, touching);

        canvas_set_color(RGB(10, 10, 30));
        canvas_clear();
        env_DrawWindow(&window);

        if (touching) {
            canvas_set_color(RGB(255, 255, 255));
            canvas_fill_circle(tx, ty, 12);
        }

        canvas_present();
    }
}