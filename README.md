# Vantage-OS

VantageOS is a Raspberry Pi version of Idkthisguy's Vantage/VantageOS.

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

Four-core cooperative task scheduling, affinity and hardware acceptance are
documented in [the scheduler guide](docs/SCHEDULER.md).

For implementation details and tests, see the [developer guide](docs/DEVELOPMENT.md).


