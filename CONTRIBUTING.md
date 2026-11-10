# Contributing

Read [the architecture guide](docs/ARCHITECTURE.md) before choosing a module.
Subsystem contracts and hardware limitations are linked from the README.

## Changes

- Keep one purpose per change and preserve unrelated behavior.
- Use the surrounding C style: four-space indentation, explicit ownership and
  bounded operations. Keep shared interfaces small and helpers local.
- Validate untrusted input at syscall/protocol boundaries. Handle partial I/O,
  overflow, timeouts and object lifetimes deliberately.
- Preserve existing ABI/on-disk formats, assembly offsets and linker contracts
  during refactors. Add a regression test for intentional behavior changes.
- Add new production sources to `mk/sources.mk`; do not link source directories
  through broad wildcards. Keep tests and historical examples out of the image.
- Regenerate generated assets using their maintained tools; retain licenses.
  Keep credentials, board-specific firmware inputs and build output out of commits.

## Verification

```sh
cmake -S . -B out/host
cmake --build out/host --config Release
ctest --test-dir out/host -C Release --output-on-failure
make BUILD_DIR=out/pi4
git diff --check
```

For SDL integration, add `-DVANTAGE_BUILD_SIMULATOR=ON` when configuring the host
build and provide SDL2. For assembly, scheduling, memory or syscall changes, run
the applicable `tests/arm/` suites against the new `kernel8.elf`, with the pinned
Unicorn/Capstone versions in the developer guide. Do not weaken warnings or
tests to hide failures. Clearly distinguish host/model tests from physical Pi
validation when describing results.
