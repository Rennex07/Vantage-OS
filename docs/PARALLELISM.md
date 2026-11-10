# Parallelism, graphics and process-management roadmap

## Current scope

Core 0 boots the system and releases cores 1 through 3 on separate stacks.
Workers enter EL1 and wait for immutable shared MMU tables through a Device
boot gate before using cached job state. A synchronous four-way batch runs one
slice on every core, including core 0. Large CPU fills and framebuffer copies
use disjoint rows; core 0 retains input/UART/mailbox/presentation ownership.
The worker facility also hosts the preemptive four-core task and process
scheduler. During task sessions, synchronous raster batches reserve idle
workers or use their serial fallback rather than wait on occupied task cores.
Batch slices run with interrupts masked and are never preempted. See
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

`platform/pi4/display.c` already requests a framebuffer and virtual-offset
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

Stages 1 to 3 are implemented in software and described in
[SCHEDULER.md](SCHEDULER.md). All of it is validated on host tests and ARM
models only; **physical Pi validation of every stage is pending**.

### 1. Four-core tasks: implemented, Pi validation pending

Thirty-two fixed task slots provide create/yield/exit/join/reap, generation-tagged
IDs and separate privileged stacks. Each of the four cores can claim runnable
work allowed by its affinity. Core 0 coordinates global quiescence; existing
device tasks stay pinned there.

### 2. Interrupt foundation and preemption: implemented, Pi validation pending

Each core initializes its GIC-400 CPU interface and virtual timer interrupt.
Exceptions save a complete frame including SIMD state. A 10 ms tick preempts
EL0 code and explicit preemptible kernel regions; all other kernel code runs
masked, so non-preemptible shared resources are never held across a switch.
The scheduler lock is only taken with interrupts masked.

### 3. Processes and address-space ownership: implemented, Pi validation pending

A process owns per-process translation tables with an ASID, its threads, file
handles and exit status. The kernel is mapped privileged-only. Syscall buffers
are resolved through the calling process's tables, faults terminate only the
offending process, and reaping flushes the ASID on every core before freeing
all memory. The terminal is the one built-in program, linked separately;
loading executables from storage would be a separate filesystem/format change.

Acceptance on hardware: two processes have independent writable memory, valid
syscalls work, invalid access faults cannot modify protected memory, a looping
process is preempted, and exit frees owned resources. The ARM model tests
check these with real translation tables; they do not replace Pi validation.

### 4. Further multicore policies

After hardware acceptance, evaluate real workloads before adding priorities,
per-core run queues, sleeping or scheduling domains. Preserve the
no-double-dispatch, safe stack-reclamation and address-space ownership
invariants when changing queue design.

GPU rendering is not a prerequisite for this process manager. Each stage should
be reviewed and validated separately rather than shipped as one large change.
