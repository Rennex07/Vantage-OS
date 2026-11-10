# Developer guide

Start with [architecture and ownership](ARCHITECTURE.md) for the source map,
dependency boundaries, build entry points and module-extension checklist.

## Simulator architecture

`include/vantage/kernel/platform.h` declares the hardware surface, and two backends implement
it:

| Backend | Files | Built by |
|---|---|---|
| HDMI framebuffer + I2C touch | `platform/pi4/display.c`, `drivers/input/touch.c`, `platform/pi4/i2c.c` | `make` → `kernel8.img` |
| SDL2 window + mouse | `sim/platform/display_sim.c` | `sim/CMakeLists.txt` → `vantage_sim.exe` |

`kernel/syscall/dispatch.c`, `kernel/ui/`, `user/apps/terminal/`
and `user/lib/user_lib.c` are shared by both targets. Hardware boot sequencing
is separate in `kernel/boot/main.c`. The simulator defines
`VANTAGE_SIM` to skip ARM user-mode startup and route syscall wrappers directly
to the same `handle_sys_call` dispatcher. `sim/main.c` starts the shared
`user_task` on desktop. This simulates the application, not ARM privilege levels
or memory protection; the Raspberry Pi build still uses its ARM startup and
`svc` instructions.

## Tests

After building the simulator, run the SDL input/rendering and mocked hardware
touch-driver tests:

```powershell
ctest --test-dir sim/build -C Release --output-on-failure
```

## Hardware user memory

The C subsystem in [`kernel/mm/`](../kernel/mm/README.md) discovers firmware RAM/reservations,
manages physical pages, and supplies the renderer's kernel heap. It runs discovery
before secondary release, finalizes reservations before MMU enablement, and keeps
the shared tables immutable. Before clearing BSS, a file-backed bootstrap stack
supports read-only firmware discovery with stack-local metadata, rejecting boot
assets that would be overwritten by zeroing. DTB bounds are checked before any
stack writes. Whole RAM holes and blocks overlapping `no-map` are
left invalid; allocation excludes all reservations. Allocator calls are restricted
to masked EL1 execution on core 0, with workers using caller-owned buffers.

User programs do not share the kernel's mappings. Every kernel block is
EL1-only: kernel text is read-only and the only memory executable at EL1, and
kernel data, page tables, stacks, the framebuffer and peripheral registers are
neither accessible nor executable from EL0. Each process has its own
translation tables for a separate user range above the 4 GiB identity map,
with read/execute text, read-only constants, and read/write data, bss and
stacks. See [the scheduler guide](SCHEDULER.md#address-spaces).

Pointer-taking syscalls check alignment, then resolve the complete buffer
through the calling process's tables and copy it to or from the kernel stack.
Misaligned buffers return 0 without reading input or drawing. A buffer the
process may not access terminates that process. The native `user_memory` test
checks the kernel mapping permissions and the syscall dispatcher with bounded
host memory; ARM permission enforcement is tested by `tests/arm/processes.py`.

## Pi 4 four-core startup

Before display initialization enables the MMU and data cache, core 0 releases
cores 1, 2 and 3 sequentially through the firmware spin-table slots at `0xe0`,
`0xe8` and `0xf0`. Each secondary checks its ID and execution state, exercises
its separate 4 KiB privileged stack, signals its own 64-byte status slot, and
then waits for the common MMU setup. Only core 0 writes UART output.
`secondary_core_start` is a one-shot boot operation, called once for each
secondary; it is not a restart API.

Core 0 waits up to one second per secondary using the Pi system timer, with an
iteration limit as a fallback. On failure, it identifies the unconfirmed core
and halts before display/MMU initialization, without releasing further cores.
A late secondary can still arrive because firmware may already have read its
release slot. These entry messages are not worker-ready or completed-job
signals. The early flags are not used for post-boot jobs.

This expects the normal 64-bit Pi 4 firmware spin-table protocol, with the MMU
and data cache disabled on all participating cores during the entry handshake.
It also relies on firmware having configured the Pi 4 CPU cluster for hardware
coherency and having loaded the image without stale cache lines. Verify on Pi 4 by checking
the serial startup message across repeated cold boots; a successful compile or
simulator test does not establish that a secondary core ran. The maintainer
reported successful hardware startup for the original core-1 implementation;
the new core-2/core-3 paths and cached worker system require hardware validation.

Record the tested commit, image hash, board and firmware version, and the full
serial log. Across repeated cold boots, expect all three messages before the
display-driver message:

```text
[OK] Core 1 entry reached
[OK] Core 2 entry reached
[OK] Core 3 entry reached
```

## Four-way CPU jobs and rendering

Each secondary enters EL1h, masks interrupts, installs the exception vectors,
enables FP/SIMD access and uses a private stack. Before accessing cached C job
state it waits on `secondary_mmu_ready`. The linker reserves a separate 2 MiB
`.smp.boot` block, mapped as privileged Device-nGnRnE memory to match MMU-off
data accesses. Core 0 builds immutable tables with caches off, enables its MMU,
then publishes this gate with a barrier/event. Workers enable the same tables,
MAIR and TCR before publishing their acquire/release worker-online signals.
Do not rebuild these shared tables after workers join.

After display/input setup, `parallel_init` waits for all three workers and
`parallel_self_test` runs eight four-way checksum batches. Each batch requires
all four core IDs to enter its callback before any can finish. Boot continues
only after the following message:

```text
[OK] Four-core parallel self-test passed
```

`parallel_run` has one bounded slot per secondary and one coordinator. Core 0
publishes callback/context before releasing each sequence number, wakes workers,
and executes slice 0 itself. Workers acquire their sequence, run their slice,
complete outstanding writes, then release completion. Core 0 acquires every
completion before returning. Contexts may live on its stack because the call
joins all work. A completion timeout halts without returning or reusing that
context. Nested submissions and submissions from a worker are rejected. The
batch facility has no dynamic queue or work stealing, and a batch slice is
never preempted; task scheduling is described below.

Large physical rectangle fills and framebuffer copies partition rows across
all four cores. Small spans stay local. Jobs receive immutable geometry/color
snapshots rather than modifying shared canvas state. Input, UART, mailbox
requests and page flips stay on core 0, after all row writes complete. These
are CPU operations, not GPU-accelerated rendering. The desktop application
renderer remains unchanged; separate SDL-thread tests execute the real batch
dispatcher and raster callbacks concurrently.

The native tests cover 256 repeated batches, core identity, serial fallback,
nested rejection, missing-worker and completion-timeout paths, and padded/odd
row counts compared with serial pixels. They do not test ARM memory ordering.
On Pi, run repeated cold boots, then sustained touch/rendering, and compare
frame output with a disposable test build that invokes the same raster callbacks
with `(part=0, parts=1)`. Record results and timings separately; no speedup is
claimed without measurement. Withhold a worker's online signal and, separately,
its job completion in disposable failure-path builds to verify bounded failure.

For a hardware failure-path test, use a disposable test build that withholds
one secondary's release. Confirm its timeout message and that display/MMU
initialization is never reached. Do not ship that test modification.

The optional assembled-code regression harness requires Python, Unicorn and
Capstone. Use a fresh build directory to avoid the repository's tracked build
artifacts:

```sh
python -m pip install unicorn==2.1.4 capstone==5.0.7
make BUILD_DIR=out/arm-validation
python tests/arm/secondary_startup.py out/arm-validation/kernel8.elf
```

It checks linked stack layout, EL1 entry, MMU register setup and gate ordering,
per-core startup, acquire/release and completion instructions, rejection paths,
timer wrap, timeout and boot sequencing. Privileged registers, timers and event
delivery are mocked; it does not test firmware, cache coherence or concurrent hardware.

For remaining worker validation, GPU and process-management stages, see the
[parallelism roadmap](PARALLELISM.md).

## Tasks, processes and preemption

`kernel/sched/` schedules tasks across all four Pi 4 cores. Each core has an
independent dispatcher context and timer tick; a locked table supplies
eligible tasks in round-robin order by immutable affinity. Kernel tasks are
trusted EL1 code. Processes own an isolated address space and run their
threads at EL0. [The scheduler guide](SCHEDULER.md) is the contract for
preemption, locking, the process lifecycle, the address-space layout, service
ownership and verification; this section is a summary.

- A 10 ms timer tick (GIC-400, per-core virtual timer) preempts EL0 code at any
  instruction, and kernel tasks only inside `task_preempt_begin` /
  `task_preempt_end`. All other kernel code runs with interrupts masked and is
  never preempted, so drivers, the allocator, storage and `parallel_run` keep
  their single-owner contracts.
- Exceptions save a complete frame (all general and SIMD registers, `SP_EL0`,
  `ELR_EL1`, `SPSR_EL1`, FPCR, FPSR) on the task's kernel stack. A preempted
  task can resume on any allowed core.
- `task_create` pins a kernel task to core 0 for device clients;
  `task_create_affinity` selects cores. Thirty-two slots have 16 KiB privileged
  stacks with a 64-byte diagnostic guard and generation-tagged IDs. Join, reap
  and `task_run` behave as before for kernel tasks.
- `process_create` builds an address space from a program image and starts its
  first thread; `process_reap` frees everything after the process has exited.
  Exit, a fault or `process_kill` ends all threads of the process only.
- The terminal is linked separately at user addresses, embedded in the kernel
  as a flat image and started as a process pinned to core 0.

Existing file/display/input services are restricted to boot or core-0-pinned
tasks; movable tasks cannot use them even while temporarily on core 0. File
syscalls reject unsupported callers with `FILE_UNAVAILABLE`. Synchronous CPU
raster jobs reserve idle secondary workers against task dispatch and retain
four-way rendering. If any secondary is occupied by a task, the renderer uses
its serial fallback on core 0. No rendering speedup is claimed.

| Syscall | ID | Result |
|---|---|---|
| `sys_task_id()` | 16 | Current thread's task ID |
| `sys_task_yield()` | 17 | One after resuming |
| `sys_task_exit(status)` | 18 | Ends the calling process; does not return |

IDs 6 to 15 are reserved for console extensions. Presentation also yields,
after the renderer and parallel jobs finish. Creating tasks and processes,
joining and waiting are kernel-only. The desktop app does not emulate
scheduling and returns zero for these three syscalls.

```sh
for suite in interrupts processes preemption tasks task_smp; do
  python tests/arm/$suite.py /path/to/fresh/kernel8.elf -v
done
```

`tasks.py` and `task_smp.py` run kernel tasks on the scripted machine with
mocked privileged instructions. `interrupts.py`, `processes.py` and
`preemption.py` run on a CPU model that executes privilege levels, the
kernel's translation tables and exception returns natively, with modeled
exception entry, GIC and timer. Native tests use four OS threads and host
fibers/ucontext with a simulated tick. None of these validate Pi firmware,
interrupt delivery, caches or timing.

Before hardware acceptance, record image/commit hashes and serial output across
cold boots. Expect `[OK] Cooperative task self-test passed`,
`[OK] Tasks executed concurrently on cores 0, 1, 2 and 3` and
`[OK] Timer ticks preempted kernel tasks on cores 0, 1, 2 and 3` before app
entry. Do not infer hardware compatibility from modeled or host-thread tests.

## Application files

Apps access a small **persistent snapshot filesystem** through `sys_file_*` in
`include/vantage/user.h` after an explicit successful storage mount. Normal Pi
boot keeps SD disabled pending physical validation, so files are unavailable
there. The simulator can mount a regular image through `VANTAGE_STORAGE_IMAGE`.
The legacy block-device RAM disk is separate. Provisioning, crash recovery,
limits and the hardware acceptance gate are documented in [STORAGE.md](STORAGE.md).

Paths are `/ram/<name>` with 1–31 ASCII letters, digits, `.`, `_` or `-`.
`.` and `..` are rejected. There are no subdirectories or relative paths. Limits
are 16 files, 4096 bytes per file, 16 open handles and 256 bytes per I/O syscall.

| Operation | ID | Payload alignment | Result |
|---|---|---|---|
| Open | 32 | `FileOpen`, 4 bytes | Positive handle |
| Read/write | 33/34 | `FileIo`, 8 bytes | Bytes transferred; zero at read EOF |
| Seek | 35 | `FileSeek`, 8 bytes | Zero on success |
| Close | 36 | Scalar handle | Zero on success |
| List | 37 | `FileEntry`, 4 bytes | One entry, or zero at end |
| Remove | 38 | `FilePath`, 1 byte | Zero on success |

Failures are negative `FileError` values. Open requires `FILE_READ` and/or
`FILE_WRITE`; `FILE_CREATE` and `FILE_TRUNCATE` require write access. Open checks
handle capacity before creating/truncating a file. A write exceeding capacity
fails without changing data or position. Seek accepts offsets through current
EOF; no sparse files are supported. Truncation leaves other handles' positions
unchanged: reads past EOF return zero, and writes there require a seek first.
The `/ram/` spelling is retained for ABI compatibility, not volatile storage.
Successful mutations commit synchronously. I/O failures quarantine the mount
until handles are released and storage is remounted; they are not guaranteed
to have left the disk unchanged. Corruption and I/O errors are reported explicitly.

Handles include a non-wrapping generation and belong to the calling process
(or to a kernel task). Exit closes that owner's handles but retains named
files. Remove rejects files with any open handles. The simulator uses one
host-app owner. Files share a namespace; handle ownership is not a
user-account permission.

Requests have no nested pointers. The dispatcher validates the full range and
alignment, snapshots input on the kernel stack, checks reserved fields and
lengths, then copies successful output back. Read clears unused payload bytes.
Operations do not yield and only core-0-pinned tasks can access them, preserving
the filesystem's single-owner rules.

```c
int64_t handle = sys_file_open("/ram/note", FILE_READ | FILE_WRITE | FILE_CREATE);
if (handle > 0) {
    char text[8];
    sys_file_write((uint64_t)handle, "hello", 5);
    sys_file_seek((uint64_t)handle, 0);
    int count = sys_file_read((uint64_t)handle, text, sizeof(text));
    /* count is a byte count; file contents are not automatically terminated. */
    (void)count;
    sys_file_close((uint64_t)handle);
}
```

For listing, start with `FileEntry entry = {0}` and call `sys_file_list(&entry)`
until it returns zero. Entries include a full path, byte size and next cursor.
Listing is not a snapshot across concurrent file creation/removal. Calls after
an error should inspect the error rather than treating it as EOF.

Native tests cover paths, permissions, capacity, stale handles, syscall buffers,
5000 deterministic read/write/seek operations against a byte-array oracle,
fresh-process persistence and interrupted snapshot commits.
They can run without SDL:

```sh
cmake -S . -B out/files-host
cmake --build out/files-host
ctest --test-dir out/files-host -L storage --output-on-failure
python tests/arm/files.py /path/to/fresh/kernel8.elf -v
```

The linked tests issue file calls from isolated EL0 processes, including the
embedded terminal's editor, and cover copied payloads, automatic cleanup when
a process exits and malformed requests, using modeled exception entry.
Physical SD read/write/erase and reboot-persistence acceptance remain pending.

## Touch events

`sys_touch_read(&event)` returns 1 when a `TouchEvent` is available, or 0 with
type `TOUCH_EVENT_NONE` otherwise. A press emits `TOUCH_EVENT_DOWN`, a change
in position while pressed emits `TOUCH_EVENT_MOVE`, and a release emits
`TOUCH_EVENT_UP`. Each event includes `x` and `y` in canvas coordinates.
The caller keeps its own pressed state between events. The hardware reports
the last known position on release and ignores failed I2C reads; it can only
detect transitions observed when polled. The desktop simulator preserves
queued mouse transitions across reads.

## Drawing UI

Optional Pi 4 V3D clear/copy and CYW43455 Ethernet transport are documented in
[the hardware driver guide](HARDWARE.md). Both are experimental and disabled at
boot by default. The V3D option accelerates full-canvas clears only after its
pixel self-test; normal shapes and presentation still use the CPU path.

`include/vantage/kernel/canvas.h` is the privileged drawing API, portable across both
builds. Its bounded `canvas_text_row` API draws 8x16 ASCII cells using a
JetBrains Mono bitmap font (SIL Open Font License). The terminal app accesses it through a validated
fixed-size syscall payload, rather than drawing directly from EL0.

`Window` in `include/vantage/kernel/window_manager.h` already carries `title[32]`,
`titleColor` and an `onTouch` callback, but nothing reads them yet. That is where
UI behaviour attaches.

## Terminal

See [the terminal guide](TERMINAL.md) for commands and input setup.

### Application and input

- `user/apps/terminal/terminal.c` owns the bounded screen model and command editing.
  Commands have a 127-character limit, reduced on smaller screens; overflow
  discards the command. Its file editor uses the existing `SYS_FILE_*` wrappers,
  keeps a 4096-byte buffer in the terminal's user-owned state, and requires a
  mounted filesystem. Its touch-first full-screen view supports in-place
  insertion, line navigation/deletion and unsaved-exit confirmation without a
  visible cursor. The disk format and file ABI are unchanged. See
  [the editor contract](TERMINAL.md#persistent-text-files).
- `user/apps/terminal/app.c` keeps its mutable state in the process's own data. It polls
  at most 64 keyboard and 64 touch events per step and redraws only dirty content.
  On the 480x800 canvas, the keyboard uses 272 pixels, leaving 33 text rows.
- Keyboard rendering and hit testing share geometry. Ordinary taps commit on
  release; dragging off cancels them. Del acts on press and repeats after 400 ms,
  then every 70 ms. Unsigned clock subtraction handles rollover; overdue repeats
  are dropped rather than replayed in a burst.
- The simulator keeps keyboard and touch queues separate. Keyboard overflow
  cancels the affected command; touch overflow resets the gesture. The terminal
  entry point disables the touch-coordinate overlay.
- `drivers/input/key.c` converts nonblocking UART input into key events. It
  accepts CR/LF/CRLF, filters unsupported ANSI navigation, and discards commands
  affected by receive overruns. Serial output mirrors terminal text. Bulk pastes
  have no flow control.

The shell accepts printable ASCII and has no history, scrollback, cursor
navigation, Unicode, quoting, pipelines or executable loading. The editor has
separate touch navigation. The terminal is the only program; there are no
PTYs and no way to start other programs from it.

`terminal_storage_reboot` exercises create/read/replace/delete using the terminal,
production syscall wrappers and a disk image across fresh processes. It also
checks exact-capacity saves, literal dot commands, discard-without-save,
unmounted-storage errors and buffer retention after a failed save. Linked ARM
file tests run the editor inside the embedded terminal process, driven by key
and touch events, and read the saved file back through the kernel. These are
software tests, not physical SD acceptance.
`editor_touch` drives SDL touch events through the application to exercise line
selection, on-screen text input, Save, New/Drop, drag cancellation, exit
confirmation and persistence after remount. Model tests exercise editor bounds
at both minimum and normal view sizes.

### Syscalls and rendering

| ID | Operation | Payload alignment |
|---|---|---|
| 6 | Poll key input (`KeyEvent`) | 4 bytes |
| 7 | Draw text row (`TextRow`) | 4 bytes |
| 8 | Read geometry/memory (`ConsoleInfo`) | 8 bytes |
| 9 | Wait for frame pacing | None |
| 10 | Write bounded serial bytes (`ConsoleWrite`) | 4 bytes |
| 11 | Draw rounded button (`CanvasButton`) | 4 bytes |
| 12 | Read wrapping 32-bit microsecond clock | None |

Payloads contain no nested pointers. The dispatcher validates full user-buffer
ranges and alignment, snapshots drawing inputs on the kernel stack, and writes
only validated outputs. Existing touch and shape syscalls remain available.

The renderer checks row, cursor, pixel bounds, radius and label size while
preserving canvas color. Button labels hold seven characters plus a terminator;
text scale is native for 0/1 and doubled for 2. `lib/memory.c` provides the
freestanding copy/zero routines needed by compiler-generated structure operations;
its Makefile flag prevents recursive library-call optimization.

Pi memory statistics come from the core-0 EL1 allocator, not per-process
accounting. The simulator reports host display allocation and does not emulate
the kernel allocator.

### Tests

CTest covers the terminal model, serial parsing, syscall buffer bounds and
SDL input-to-pixel rendering, including key feedback, scaled glyphs and drag
cancellation. Model tests also run 20,000 deterministic mixed input events.

With Unicorn/Capstone installed and a fresh AArch64 build:

```sh
python tests/arm/terminal.py /path/to/kernel8.elf -v
```

The linked tests run the embedded terminal as an isolated, preempted EL0
process and cover its stack bounds, syscall validation, memory statistics,
copy/zero routines and Del repeat timing, including rollover and empty input.
Exception entry and devices are modeled. Validate touch coordinates, serial/display
output, scrolling and memory counters on a physical Pi before claiming hardware
compatibility.

### Font regeneration

Retain `drivers/video/OFL.txt` when distributing the JetBrains Mono bitmap.
Pillow is needed only to regenerate the checked-in header:

```sh
python -m pip install pillow
python drivers/video/gen_font8x16.py --ttf /path/to/JetBrainsMono-Regular.ttf
```
