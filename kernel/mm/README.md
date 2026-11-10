# Kernel memory management

The C memory subsystem discovers firmware RAM and reservations, manages 4 KiB
physical pages, and supplies an 8 MiB aligned/coalescing kernel heap. The hardware
renderer obtains its back buffer from the heap. It builds with the existing GNU
AArch64 toolchain.

This is physical-memory and kernel-heap management. Process address spaces are
built on top of it by the scheduler's process layer, one page at a time (see
[the scheduler guide](../../docs/SCHEDULER.md#address-spaces)). There is no
demand paging, swapping, heap growth or GPU command submission.
GPU-visible addressing, buffer/cache ownership and completion synchronization
must be designed for the graphics interface before using these allocations as
GPU buffers.

## Boot order

1. `boot.s` preserves the firmware's DTB pointer in `x0` and masks exceptions.
   Before any memory writes, assembly rejects a DTB overlapping the kernel's
   complete runtime footprint and checks its alignment, magic, size and numeric
   bounds. On a 32 KiB file-backed bootstrap stack, `mm_boot_validate` reuses the
   bounded C parser to check RAM coverage and reject reservation-map, static
   reserved-memory and initrd ranges overlapping the BSS clearing interval. It
   uses stack-local metadata, not uninitialized globals. Only after this succeeds
   does boot select the primary BSS stack, clear BSS and enter `main`. Early
   rejection parks the core without initializing UART or writing BSS.
2. `mm_prepare` parses a bounded DTB and records RAM, its reservation map,
   `/reserved-memory` and `/chosen` initrd ranges. Low memory through `_end`
   (including metadata, stacks, the embedded program image and the
   secondary-core boot gate), and the DTB itself, are reserved.
3. Firmware allocates the framebuffer. `mm_finalize_framebuffer` reserves its
   **full returned allocation**, rounded outward to 2 MiB mapping boundaries,
   before resolving dynamic reserved-memory pools and their alignment/alloc-range
   constraints. Reusable pools remain reserved.
4. C builds the immutable, privileged-only identity tables using `mm_block_mapped`. Entire blocks
   outside RAM and those intersecting `no-map` stay invalid, except that firmware
   framebuffer memory can be mapped outside the CPU RAM banks. Conflicting
   `no-map` reservations over the kernel, DTB or framebuffer stop boot. Sub-block
   RAM holes can remain mapped, but never supply allocatable pages. The reserved
   secondary-core boot gate retains its privileged Device mapping.
5. The primary enables the MMU/caches and releases the existing MMU-ready gate.
   Secondary cores enable the same tables and enter their workers; they do not
   access allocator metadata. Process tables created later share these kernel
   entries and never change them.
6. `mm_init_allocator` initializes out-of-band page metadata and reserves an
   8 MiB page run for the heap. `mm_self_test` checks real memory writes, alignment,
   invalid frees and accounting. The renderer then allocates a zeroed back buffer.

Discovery/finalization failures are terminal for that boot. Missing or unsupported
firmware descriptions are not replaced with guessed RAM sizes. Firmware must
load the file-backed kernel image (including the bootstrap stack) without
overlapping other firmware assets and provide a readable, immutable DTB. Boot
cannot recover data already overwritten by the firmware's image load. The early
check protects assets from the kernel's subsequent BSS clearing and stack writes;
reservations over that zero-fill tail fail closed. Numerical pointer checks cannot
prove physical readability. Preserve
`-mstrict-align`: discovery runs before the MMU, where unaligned multi-byte Device
accesses can fault.

## Ownership and allocation API

See [`include/vantage/kernel/memory.h`](../../include/vantage/kernel/memory.h). Calls require core 0, EL1 and
all DAIF exception masks set. Other cores and unmasked callers are rejected before
metadata access. The nonblocking reentry guard uses plain accesses because
exclusive instructions are unsuitable for MMU-off Device accesses. It is **not a
multicore lock**. Interrupt-enabled or multicore allocation needs a new ownership
protocol, not removal of the checks. Timer preemption does not change this:
kernel code runs with interrupts masked, so a tick cannot interrupt an
allocator call, and interrupt-enabled kernel regions are rejected like any
other unmasked caller. Process creation and teardown, the only users of
`mm_alloc_pages`/`mm_free_pages` besides drivers, run on core 0 for this reason.

Workers receive preallocated buffers. Join every user of a buffer before freeing
it. The existing synchronous parallel dispatcher provides this join for renderer
jobs.

- `mm_alloc_pages(count, alignment_pages)` returns a physical/identity address.
  Count must be nonzero and alignment a nonzero power of two. `mm_free_pages`
  requires the exact starting address and run length. Partial, interior, reserved
  and double frees are rejected. The heap's backing run cannot be freed this way.
- `mm_alloc(size, alignment)` returns an uninitialized kernel buffer; `mm_zalloc`
  zeroes it. Invalid requests, capacity exhaustion and unavailable memory return
  null. `mm_free` requires the exact live allocation's start.
- `mm_stats` writes a `MemoryStats` to exclusive, writable, aligned ordinary kernel
  memory. The pointer must not refer to user memory or allocator metadata.
- Freed memory is not automatically scrubbed. A raw free cannot distinguish an
  old pointer from a newer allocation reusing the address. Callers remain
  responsible for avoiding stale pointers, use-after-free and out-of-bounds access.

Page metadata records reserved/free/head/continuation states outside the pages.
Free validates the full run before changing anything. Heap metadata is outside
the arena; sorted free extents coalesce. Capacity is reserved so freeing a
successful allocation does not require new metadata.

## Bounds

- Only firmware-described RAM below `0xfc000000` is allocatable. The current
  identity tables cover the first 4 GiB; higher Pi RAM is excluded.
- 128 coalesced ranges per RAM/reservation/no-map set, 32 DTB nesting levels,
  16 dynamic pools with up to 8 constraints each, a 2 MiB DTB limit, 255-byte node
  names and 31-byte property names. Exhausting capacity fails closed. Parsing is
  iterative with bounded arrays and name scans.
- The heap retains an 8 MiB run for kernel lifetime and 1,024-entry free/live
  metadata arrays. Fragmentation or metadata pressure can exhaust it before its
  byte capacity. First-fit searches have no constant-time or speedup guarantee.
- Page metadata consumes roughly 4 MiB of BSS; the primary stack is 64 KiB. The
  32 KiB bootstrap stack is included in the loaded image and retained for kernel
  lifetime. Discovery runs twice, before BSS clearing and during MM preparation.
- Nonempty reserved-memory translations and `linux,usable-memory*` crash-kernel
  restrictions are rejected, rather than silently ignored.
- The identity tables use 2 MiB blocks: kernel text and constants share one
  read-only block that is executable at EL1, and every other block is
  non-executable. No kernel block is accessible or executable from EL0. Finer
  kernel W^X (separating text from constants) is not implemented.
- Process pages come from `mm_alloc_pages` one at a time and are not scrubbed
  when freed; the process layer zeroes every page it maps.

## Build and validation

Use an external build directory so tracked upstream artifacts are not overwritten:

```sh
make BUILD_DIR=/path/to/arm-build
# With Arm's bare-metal toolchain:
make BUILD_DIR=/path/to/arm-build CC=aarch64-none-elf-gcc \
  LD=aarch64-none-elf-ld OBJCOPY=aarch64-none-elf-objcopy
```

The simulator CMake build registers `mm_pages`, `mm_allocators` and `mm_fdt` with
CTest. They compile the real allocator/discovery code with warnings as errors.
Allocator tests use independent byte/bitmap models over 20,000 operations each,
test fragmentation and metadata pressure, and require full heap recovery. DTB
tests cover reservations, 32/64-bit cells, dynamic pools, truncation, malformed
inputs, capacity limits and deterministic byte mutations.

The same host tests can be built independently of the simulator:

```sh
cmake -S . -B /path/to/mm-host -DCMAKE_BUILD_TYPE=Debug
cmake --build /path/to/mm-host
ctest --test-dir /path/to/mm-host -L memory --output-on-failure
```

On MSVC's multi-configuration generator, add `--config Release` to the build and
`-C Release` to CTest. The memory CI also runs these native tests with AddressSanitizer
and UndefinedBehaviorSanitizer on Linux.

```sh
python -m pip install unicorn==2.1.4 capstone==5.0.7
python tests/arm/memory_management.py /path/to/arm-build/kernel8.elf -v
python tests/arm/secondary_startup.py /path/to/arm-build/kernel8.elf -v
```

The MM harness runs the linked C allocator, boot path and renderer, checking
stack guards, zeroing, ownership, reservations, page-table entries and gate
publication. Renderer checks use the dispatcher's serial fallback; the desktop
thread tests separately exercise the real dispatcher and raster callbacks.
Privileged registers, firmware, timers and event behavior are mocked. These
checks do not establish real MMU/cache behavior or Pi hardware compatibility.
Startup regressions place DTBs and initrd/reserved payload sentinels in BSS,
the primary stack and the secondary gate. They require rejection
without writes outside the file-backed bootstrap stack, preservation of the
original payload bytes, and no stores at all for DTBs overlapping kernel memory.

On Pi 4, record the commit/image hashes, board/firmware details and serial log.
Repeat cold boots and look for:

```text
[OK] Firmware RAM and reservations discovered
[OK] Page allocator and heap self-test passed
[OK] Four-core parallel self-test passed
```

Then exercise sustained touch/rendering, compare frame output, and test allocator
exhaustion/reuse in disposable images. The C MM integration still needs this
hardware validation, independently of earlier four-core hardware results.

## References

- [DTB layout and reservation map](https://devicetree-specification.readthedocs.io/en/stable/flattened-format.html)
- [Memory nodes and cell sizes](https://devicetree-specification.readthedocs.io/en/stable/devicenodes.html)
- [Reserved-memory binding](https://github.com/devicetree-org/dt-schema/blob/main/dtschema/schemas/reserved-memory/reserved-memory.yaml)
- [Pi firmware boot stub](https://github.com/raspberrypi/tools/blob/master/armstubs/armstub8.S)
- [AArch64 boot protocol](https://www.kernel.org/doc/Documentation/arm64/booting.txt)
