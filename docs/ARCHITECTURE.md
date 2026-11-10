# Codebase structure and ownership

Vantage has one set of application/kernel services and two platform backends:
the bare-metal Pi 4 and an SDL2 desktop simulator. The simulator is not an ARM
emulator. Keep hardware ownership and privilege checks in place when sharing code.

## Directory map

```text
arch/armv8/            AArch64 boot, exception entry, context switching and MMU
include/vantage/
  abi/                Shared syscall numbers, value types and fixed-size payloads
  arch/               Architecture interfaces
  drivers/            Driver/protocol interfaces
  kernel/             Privileged services and architecture-port contracts
  platform/pi4/       Pi 4 board interfaces and register definitions
  user.h              Userspace syscall wrappers
kernel/
  boot/               Hardware boot coordinator and built-in application launch
  fs/                 File handles, snapshot storage and file syscall validation
  mm/                 Firmware discovery, physical pages and kernel heap
  parallel/           Synchronous multicore worker coordination
  sched/              Cooperative task lifecycle and scheduling
  syscall/            Common validated syscall dispatcher
  ui/                 Canvas, render backend seam, row rasterization and window geometry
drivers/
  hw/                 Bounded hardware polling and property-mailbox transactions
  input/              Touch and serial-key event decoding
  storage/            Portable SD command/state engine
  video/              V3D engine and render backend, licensed generated font assets
  wifi/               SDIO/CYW43455 protocol and firmware transport
platform/pi4/         MMIO, clocks, display, SD and storage adapters, driver startup
lib/                  Canonical freestanding memory primitives
user/
  apps/terminal/      Terminal/editor model and interactive application
  lib/                Userspace syscall transport and wrappers
sim/
  platform/           Host display/input and regular-image sector adapters
  tests/              SDL integration and concurrent raster tests
tests/
  native/             Headless driver, filesystem, memory, scheduler and ABI tests
  support/            Shared test fixtures and device models
  arm/                Linked AArch64 execution/model regressions
  tools/              Image, firmware, configuration and layout-tool tests
mk/                   Hardware feature options and explicit production manifest
cmake/                Shared host build/warning and SDL2 setup
tools/                Image provisioning, firmware embedding and build utilities
legacy/               Retained historical examples, excluded from production
```

## Dependency rules

- Include public project headers as `<vantage/...>`, using only `-Iinclude`.
  Use quoted local headers for subsystem-private types, such as memory metadata
  and terminal state. Tests may inspect those private headers explicitly.
- `abi/` and `user.h` must not import kernel, architecture, driver or platform
  APIs. ABI structures are shared data, not kernel service declarations. Userspace
  applications call `sys_*` wrappers; they do not call raw drawing/file/MMIO APIs.
- `kernel/syscall/dispatch.c` validates userspace requests and routes them to
  privileged services. `kernel/fs/file_syscall.c` snapshots file payloads.
  Keep validation at this boundary; internal file APIs accept trusted buffers.
- `kernel/boot/main.c` owns boot sequencing. The desktop build shares the syscall
  dispatcher and app, not the privileged boot coordinator.
- Portable command engines receive callbacks (`HwOps`, `SdIo`, `SectorDevice`).
  The Pi backend owns MMIO/cache maintenance and the host backend owns files/SDL.
  Device-specific register maps inside protocol drivers are intentional.
- `include/vantage/kernel/render.h` is the only seam between the display layer
  and its renderers. `platform/pi4/startup.c` selects the V3D backend at
  runtime after its self-test; canvas and display code never call the V3D
  engine directly, and CPU raster stays available behind the same seam.
- The scheduler and parallel ports describe architecture seams, not userspace
  APIs. Module-local globals retain their current single-owner/locking contracts.
- `lib/memory.c` is the production implementation of `memcpy`, `memset`, and
  `memcmp`. Do not introduce competing definitions in drivers or filesystems.

These boundaries do not introduce process isolation, preemption, reentrant
storage, or new hardware support. See the subsystem guides for lifetime,
core-affinity, exception-mask and error-recovery requirements.

## Build entry points

Hardware continues to use GNU Make and the existing feature flags:

```sh
make BUILD_DIR=out/pi4
make BUILD_DIR=out/cpu V3D=0
```

The default image probes V3D at boot and falls back to CPU rendering;
`V3D=0` builds the CPU renderer only. Wi-Fi and storage validation remain
opt-in.

The output remains `<BUILD_DIR>/prod/kernel8.img` with a linked
`<BUILD_DIR>/kernel8.elf`. `mk/config.mk` validates the build switches;
`mk/sources.mk` explicitly lists production sources. Add each new `.c`/`.s` there
exactly once. Do not restore recursive wildcards: they can accidentally link
test doubles, alternate runtimes or incomplete examples. Configuration stamps
and header dependency files preserve option-sensitive incremental rebuilds.

Host tests have one SDL-independent entry point:

```sh
cmake -S . -B out/host
cmake --build out/host --config Release
ctest --test-dir out/host -C Release --output-on-failure
ctest --test-dir out/host -C Release -L storage --output-on-failure
```

Labels are `drivers`, `memory`, `storage`, `scheduler`, `user`, `tooling`, and
`simulator`. Enable the desktop application/integration tests with
`-DVANTAGE_BUILD_SIMULATOR=ON` and supply SDL2 through `CMAKE_PREFIX_PATH` when
needed. The documented standalone `cmake -S sim` entry point is also supported.
The former per-subsystem CMake entry points are replaced by the root build and
CTest labels. Both forms share the helpers in `cmake/`.

Use new build directories after this source-layout change. Existing upstream
`build/` and `out/` artifacts are not authoritative source or validation evidence.

### Source migration

The old `includes/` and `user/include/` paths are replaced by the canonical
namespace. Internal/external source consumers should update their includes:

| Previous header | New include |
|---|---|
| `user/include/user_lib.h` | `<vantage/user.h>` |
| `includes/files.h` | `<vantage/abi/files.h>` |
| `includes/task.h` | `<vantage/kernel/task.h>` |
| `includes/memory.h` | `<vantage/kernel/memory.h>` |
| `includes/video/graphics.h` | `<vantage/kernel/canvas.h>` |
| `includes/sd.h` command engine | `<vantage/drivers/sd.h>` |
| `includes/sd.h` Pi adapter | `<vantage/platform/pi4/sd.h>` |

Input/graphics payloads live in `abi/`; raw driver operations live in `drivers/`
or `platform/pi4/`. The include paths change; existing syscall numbers, payloads,
function names and on-disk encodings remain unchanged.

## Extending a subsystem

1. Choose its owner: shared ABI, privileged service, device protocol, board
   adapter, application, or host backend. Avoid adding generic manager layers
   when a subsystem-local helper is sufficient.
2. Declare only cross-module interfaces in `include/vantage/`; keep private
   state and helper types alongside their implementation.
3. Update the production manifest and the relevant native test target. Reuse the
   host warning policy in `cmake/VantageHost.cmake`.
4. Preserve syscall IDs, error values, payload layout, on-disk encoding, linker
   sections and assembly entry symbols unless explicitly changing that contract.
5. Run the affected native tests and linked ARM regressions. Exercise default,
   `V3D=0` and opt-in builds when changing hardware startup or configuration.
6. Update the subsystem guide and keep licensed/generated assets tied to their
   generators and applicable notices. Hardware acceptance still needs Pi logs.

`syscall_abi` asserts existing payload layouts/numbers. Python layout tests check
the ABI/application dependency boundaries, canonical includes and exact source
manifest coverage. CI retains sanitizer coverage and the separate four-thread
scheduler UBSan run.
