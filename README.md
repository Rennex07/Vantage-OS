# Vantage-OS

VantageOS is a Raspberry Pi version of Idkthisguy's Vantage/VantageOS

## Desktop simulator

Runs the same UI on desktop for those who want to work on Development without PI.

### Prerequisites

- CMake
- An **SDL2 Visual C++** package: the latest `SDL2-devel-*-VC.zip` from
  [libsdl-org/SDL releases](https://github.com/libsdl-org/SDL/releases).

### Build and run

From the repository root:

```powershell
cmake -S sim -B sim/build -DCMAKE_PREFIX_PATH=C:\path\to\SDL2-devel-2.32.10-VC
cmake --build sim/build --config Release
.\sim\build\Release\vantage_sim.exe
```

CMake finds Visual Studio on its own, and `SDL2.dll` is copied next to the
executable, so the simulator runs straight from the build directory.

### How it works

`includes/platform.h` declares the hardware surface, and two backends implement
it:

| Backend | Files | Built by |
|---|---|---|
| HDMI framebuffer + I2C touch | `src/drivers/display.c`, `touch.c`, `I2C.c` | `make` → `kernel8.img` |
| SDL2 window + mouse | `sim/platform/display_sim.c` | `sim/CMakeLists.txt` → `vantage_sim.exe` |

`src/main.c`, `src/drivers/graphics.c` and `src/environment/windowManager.c`
compile into both targets unchanged, so the simulator runs the same application
code the Pi does. Do not merge the two `display` implementations — that is what
keeps the simulator working.

### Drawing UI

`includes/graphics.h` is the drawing API and is portable — call it from either
build. It draws shapes only, so a bitmap font baked into a C array and a
`canvas_draw_text` blitter are needed before anything can be labelled.

`Window` in `includes/environment/windowManager.h` already carries `title[32]`,
`titleColor` and an `onTouch` callback, but nothing reads them yet. That is where
UI behaviour attaches.