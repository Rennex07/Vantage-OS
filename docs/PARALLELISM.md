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
[SCHEDULER.md](SCHEDULER.md). This worker facility is the CPU
renderer; the default V3D backend is described below. The original core-1 startup
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

## GPU: V3D backend and the CPU renderer

V3D 4.2 is the default rendering backend of the Pi 4 image. Full-surface
clears and presents run as V3D jobs; every other operation is CPU raster on
the same surface. Bring-up, memory ownership, synchronization and fallback
rules are in the [hardware driver guide](HARDWARE.md). It is validated against
host models and Unicorn only: physical Pi validation is pending and no
speedup is claimed.

The four-way CPU raster stays fully functional. It is the whole renderer in
`V3D=0` images, after a failed boot probe and after a runtime GPU failure, and
it draws partial fills in every configuration. Ordering rules:

- Core 0 owns the display. A V3D job is submitted and completed inside one
  call on core 0 with exceptions masked; a raster batch joins its workers
  before `parallel_run` returns. The two never overlap on a surface, and no
  job is submitted from a batch callback.
- Worker reservation is unchanged: during a task session a batch reserves
  idle workers or runs its serial slice. V3D jobs do not use workers and do
  not wait on task cores.
- A V3D timeout or fault stops the device, moves the frame to the CPU back
  buffer and repeats the operation on the CPU, so the frame stays correct.
- With timer preemption, each renderer call, including its V3D job or raster
  batch, must stay non-preemptible and must not be re-entered.

`platform/pi4/display.c` still requests the framebuffer and page flips from
VideoCore firmware through the property mailbox. VideoCore-backed display
output does not establish that V3D rendered anything; the boot self-test and
the UART backend message report that, so far only against software models.

The Pi 4 uses V3D 4.2. Mesa and Linux implement separate rendering and display
drivers; this driver covers only the buffer ownership, GPU page tables,
command submission and completion/error handling needed for 2D clear and
present. It has no shader code. Do not use old VC4 QPU tutorials as proof of
Pi 4 compatibility.

Next GPU steps: complete the physical acceptance checklist in the hardware
guide, compare pixels and frame times with a `V3D=0` image, and only then
consider moving further operations to the device.

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
