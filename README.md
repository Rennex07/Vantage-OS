# Vantage-OS

VantageOS is a Raspberry Pi version of Idkthisguy's Vantage/VantageOS.

## Raspberry Pi 4 kernel

With Python 3, GNU Make and the GNU AArch64 cross-toolchain installed:

```sh
make BUILD_DIR=out/pi4
```

The kernel image is `out/pi4/prod/kernel8.img`; the linked test image is
`out/pi4/kernel8.elf`. The kernel image is not a complete bootable SD-card image.
V3D, Wi-Fi and storage validation remain explicit build-time options; see
[the hardware guide](docs/HARDWARE.md) and [storage guide](docs/STORAGE.md).

## Desktop simulator

You can run the simulator on a Windows PC without a Raspberry Pi.

### Prerequisites

- [CMake](https://cmake.org/download/)
- Visual Studio or Visual Studio Build Tools with C++ support
- The bundled SDL2 Visual C++ package in `sim/SDL2`.

### Build and run

Open PowerShell in the repository folder, then run:

```powershell
cmake -S sim -B sim/build -DCMAKE_PREFIX_PATH="$PWD/sim/SDL2"
cmake --build sim/build --config Release
.\sim\build\Release\vantage_sim.exe
```

Type `help` to get started, using your keyboard or the on-screen keys. With a
storage image mounted, `edit`, `cat`, `ls` and `rm` read and write persistent
files. See the [terminal guide](docs/TERMINAL.md) for usage.

Experimental persistent SD files and the physical Pi validation gate are
documented in [the storage guide](docs/STORAGE.md).

Preemptive four-core scheduling, processes with isolated address spaces and
their hardware acceptance checklist are documented in
[the scheduler guide](docs/SCHEDULER.md). They are validated on host tests and
ARM models; physical Pi validation is pending.

For implementation details and tests, see the [developer guide](docs/DEVELOPMENT.md).

## Headless tests and source layout

Requires CMake, a host C compiler and Python 3.11+.

```sh
cmake -S . -B out/host
cmake --build out/host --config Release
ctest --test-dir out/host -C Release --output-on-failure
```

Add `-DVANTAGE_BUILD_SIMULATOR=ON` and an SDL2 package path to include the desktop
application and rendering tests in the root build. Headless tests do not require
SDL2. Use CTest labels such as `-L storage` or `-L scheduler` for a focused run.

See [architecture and ownership](docs/ARCHITECTURE.md) for the directory map and
[contributing](CONTRIBUTING.md) for change and verification conventions.
