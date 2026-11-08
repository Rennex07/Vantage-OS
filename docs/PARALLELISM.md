# Parallelism, graphics and process-management roadmap

## Current scope

Core 0 boots the system and releases cores 1 through 3 on separate stacks.
Workers enter EL1 and wait for immutable shared MMU tables through a Device
boot gate before using cached job state. A synchronous four-way batch runs one
slice on every core, including core 0. Large CPU fills and framebuffer copies
use disjoint rows; core 0 retains input/UART/mailbox/presentation ownership.
The worker facility also hosts a cooperative four-core task scheduler. During
task sessions, synchronous raster batches reserve idle workers or use their
serial fallback rather than wait on occupied task cores. See
[SCHEDULER.md](SCHEDULER.md). This is not a GPU
renderer. The original core-1 startup
was reported working on hardware by the maintainer; the new worker system
still needs Pi 4 runtime and cache-coherency validation.

## Next: validate and extend parallel work

1. Validate all four EL1/MMU/cache configurations on Pi hardware. Keep the early
   entry flags separate from post-boot acquire/release job state.
2. Validate the boot checksum barrier and sustained batches, including missing
   workers, completion timeouts and wakeups around `wfe`/`sev`.
3. Compare parallel raster output and measured frame times against serial
   execution before claiming performance improvements.
4. Add a more general queue or load balancing only when real workloads need
   them. The current single-owner batch rejects nested submission instead of
   blocking on a full queue. Keep callbacks bounded and independent; no
   allocation or blocking driver calls inside jobs.

Acceptance: repeated Pi cold boots and sustained parallel correctness tests,
with commit/image hashes and full logs. Desktop threads or mocked ARM tests
must be labelled separately from hardware results.

## GPU: existing display versus acceleration

An experimental V3D 4.2 clear/copy driver now provides private GPU mappings,
bounded command submission and an opt-in pixel self-test. See the
[hardware driver guide](HARDWARE.md). Physical Pi validation remains pending;
the default renderer still runs on the CPU.

`drivers/video/display.c` already requests a framebuffer and virtual-offset
changes from VideoCore firmware through the property mailbox. Drawing and
framebuffer copying run on the ARM CPU. VideoCore-backed display output does
not establish that V3D renders any shapes or executes compute jobs.

The Pi 4 uses V3D 4.2. Mesa and Linux implement separate rendering and display
drivers; enabling a V3D clock is not a replacement for buffer ownership, GPU
page tables, command submission, completion/error handling and valid shader
code. A V3D driver is a separate project, not part of secondary-core startup.

Recommended first GPU milestone, if pursued: identify the hardware and its
power/clock requirements, reserve explicitly owned buffers, and execute one
bounded off-screen render with a checked pixel result before connecting it to
display presentation. Establish timeout/recovery behavior before submitting
arbitrary workloads. Keep the existing CPU renderer usable for comparison.
Do not use old VC4 QPU tutorials as proof of Pi 4 compatibility.

References:

- [Pi firmware spin-table implementation](https://github.com/raspberrypi/tools/blob/master/armstubs/armstub8.S)
- [Firmware mailbox property interface](https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface)
- [Mesa V3D driver and Pi 4 hardware notes](https://docs.mesa3d.org/drivers/v3d.html)
- [Linux V3D driver: memory, jobs and interrupts](https://docs.kernel.org/gpu/v3d.html)

## Task scheduler and process-management stages

Start with kernel task lifecycle and scheduling, not a graphical task-manager
application. The kernel schedules trusted cooperative tasks and starts one
built-in EL0 function; it has no executable loader, timer-driven scheduler or
per-process address spaces. The existing shared code mapping and single user-memory region
are not independent process isolation.

### 1. Four-core cooperative tasks: implemented, Pi validation pending

Thirty-two fixed task slots provide create/yield/exit/join/reap, generation-tagged
IDs and separate privileged stacks. Each of the four cores can claim runnable
work allowed by its affinity, with migration at voluntary yield/block boundaries.
Core 0 coordinates global quiescence; existing device tasks stay pinned there.
EL0 exposes ID/yield/exit only. See [the task contract and tests](SCHEDULER.md).
Host threads and linked ARM models cover progress, migration, preserved context
and safe slot reclamation; physical Pi scheduling validation is still required.

### 2. Interrupt foundation and preemption

Add GIC/timer setup, interrupt acknowledgement and safe exception entry before
preempting tasks. Specify interrupt-safe locking and when scheduling is allowed.
Save the complete state required by the selected execution mode, including
exception-return registers and an explicit FP/SIMD ownership policy. Test timer
interrupts during syscalls and driver operations; do not switch tasks while a
non-preemptible shared resource is held.

### 3. Processes and address-space ownership

Add a physical-page allocator and explicit per-process code/data/stack mappings
with privileged kernel mappings. Define how syscall buffers are validated
against the current process, how faults terminate a task safely, and how memory
is reclaimed. Start with built-in test programs; executable loading can follow
as a separate filesystem/format change.

Acceptance: two processes have independent writable memory, valid syscalls work,
invalid access faults cannot modify protected memory, and exit frees owned
resources. Verify this on ARM, not only by inspecting page-table descriptors.

### 4. Further multicore policies

After hardware acceptance, evaluate real workloads before adding priorities,
per-core run queues or scheduling domains. Process migration will need explicit
address-space/TLB ownership beyond the current shared-map cooperative model.
Preserve the no-double-dispatch and safe stack-reclamation invariants when adding
preemption or changing queue design.

GPU rendering is not a prerequisite for this process manager. Each stage should
be reviewed and validated separately rather than shipped as one large change.
