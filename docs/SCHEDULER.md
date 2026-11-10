# Preemptive four-core scheduling, processes and address spaces

## Status and scope

The scheduler runs tasks on Pi 4 cores 0–3. Each core has a private dispatcher
context, an exclusive current-task slot and its own timer tick. One locked,
bounded task table provides round-robin selection by immutable affinity.

- **Kernel tasks** are trusted EL1 code with the kernel's address space only.
- **Processes** own an isolated address space, one to four threads that run at
  EL0, their file handles and an exit status. The built-in terminal is a
  process.
- **Preemption** is driven by a per-core timer interrupt with a 10 ms slice.

**Physical Pi acceptance is pending.** Everything here is validated on host
tests and on ARM CPU models only. Those check software behaviour; they do not
check firmware handoff, real interrupt delivery, the GIC, timer rates, cache or
TLB behaviour, or speed. See [Verification](#verification) and the
[acceptance checklist](#physical-acceptance-checklist).

There are no priorities, signals, sleeping or timed wakeups, demand paging,
`fork`, executable loading from storage, user-created threads or processes, or
per-user permissions. A single function is not split across cores
automatically: callers submit independent work.

## Preemption policy

| Context | Interrupts | Preempted by the tick |
|---|---|---|
| EL0 code of a process thread | Always enabled; EL0 cannot mask them | Yes, at any instruction |
| Kernel task inside `task_preempt_begin` … `task_preempt_end` | IRQ enabled | Yes, at any instruction |
| Any other EL1 code: syscalls, fault handling, kernel tasks, dispatchers, boot, drivers, `parallel_run` jobs | All masked | No |

The kernel is therefore not preemptible, except in regions a kernel task opens
explicitly. A tick that expires while the kernel is running stays pending and
is taken at the first unmasked instruction, normally the return to EL0.

Consequences:

- A process that loops forever, or spends its whole slice in system calls,
  loses its core at the end of the slice. It cannot mask interrupts, stop or
  reprogram the timer (`CNTKCTL_EL1` is cleared on every core) or reach the
  interrupt controller.
- A kernel task that neither yields nor opens a preemptible region still keeps
  its core. Kernel tasks are trusted.
- Inside a preemptible region a kernel task must hold no kernel resource and
  call nothing but `task_preempt_end`. Scheduler, process, allocator, storage,
  mailbox and display services all require masked EL1 and reject the caller.
  The region is for bounded pure computation on task-owned data.

```c
static void compute(void* argument) {
    Work* work = argument;                 /* Owned by this task. */
    if (task_preempt_begin() != TASK_OK) task_exit(-1);
    crunch(work);                          /* May be preempted and migrated. */
    task_preempt_end();                    /* Masked again: services allowed. */
    task_exit(0);
}
```

Do not cache the core ID across a preemptible region, a yield or a block.

## Interrupts and the timer

`arch/armv8/irq.c` drives the Pi 4 GIC-400 (GICv2: distributor `0xff841000`,
CPU interface `0xff842000`) and the EL1 **virtual** timer, PPI 27. The virtual
timer needs no EL2 permission when the kernel is entered at EL1; when it is
entered at EL2, `boot.s` and `secondary.s` grant EL1 the timers
(`CNTHCTL_EL2 = 3`) and zero `CNTVOFF_EL2` before dropping to EL1.

Each core calls `irq_init_core` once with the MMU on: core 0 from the boot
coordinator, a secondary before it joins the workers. It clears `CNTKCTL_EL1`,
enables Group 1 forwarding in the distributor (core 0), sets the priority of
and enables its banked PPI 27, opens the priority mask and enables its CPU
interface. The timer is left stopped.

This relies on the default Pi 4 firmware handoff (`enable_gic=1`, the stock
ARM stub): the stub runs in the secure state and assigns all interrupts to
non-secure Group 1 before entering the kernel. If the timer interrupt cannot
be enabled from the kernel's view, or `CNTFRQ_EL0` is zero, `irq_init_core`
fails: core 0 halts the boot and a secondary parks before it runs any task.

The timer is one-shot. The dispatcher arms `TASK_SLICE_US` (10 ms) before it
switches to a task and stops the timer when the task returns to it, so an
idle or dispatching core has no interrupt source. The handler stops the
level-triggered timer, completes the interrupt in the GIC on the core that
acknowledged it, and only then calls the scheduler; the task may resume on
another core. Spurious interrupt IDs are ignored.

### Trap frame

Every exception taken to EL1 from EL0 (system calls, faults, interrupts) and
every interrupt taken from an interrupt-enabled EL1 region saves a complete
`TrapFrame` (`include/vantage/arch/trap.h`, 800 bytes) on the interrupted
task's kernel stack: x0–x30, `SP_EL0`, `ELR_EL1`, `SPSR_EL1`, FPCR, FPSR and
the full 128-bit v0–v31. `arch/armv8/trap.c` asserts the layout that
`arch/armv8/exception.s` uses. The return path restores everything from the
frame.

A tick preempts by yielding from inside the interrupt handler: the frame stays
on the task's kernel stack and the existing register switch saves the handler's
call-boundary context. Preempted, yielded and blocked tasks therefore look the
same to the dispatcher, and a preempted task can be resumed by any allowed
core. A thread first enters EL0 through the same return path from a zeroed
frame, so no kernel register value reaches user code.

Interrupts never nest. A synchronous exception inside the kernel, FIQ and
SError are not recoverable and park the core.

## Tasks

Initialize after memory/MMU setup, secondary startup and `parallel_init`.
`task_create` pins work to core 0 for compatibility with device services;
`task_create_affinity(entry, argument, mask)` selects cores (`TASK_ALL_CORES`,
or `1U << n`). Affinity is immutable. Affinity zero, out-of-range bits and
unavailable cores are rejected. Without the four-core worker facility only
affinity 1 is available.

There are 32 task slots with 16 KiB privileged stacks, diagnostic stack guards
and non-wrapping generation-tagged IDs. Tasks are runnable, running, blocked
or exited. `task_yield` returns to the core's dispatcher. Returning from a
kernel task's entry exits with status zero. `task_join` blocks a running
kernel task, rejects self/cyclic waits and reserves the target for one waiter;
`task_reap` is nonblocking and also works from boot. An exited task holds its
slot until it is reaped. `task_info` snapshots state, affinity, current core
and the dispatch and preemption counts. `task_run` on core 0 returns when all
cores are quiescent; blocked-only state returns `TASK_PENDING`. It does not
return while a process is running.

Arguments and status pointers are trusted kernel pointers. An argument must
outlive every task that uses it. Threads of a process are tasks owned by that
process: `task_join` and `task_reap` reject them.

## Locking

One spin lock protects the task table, cursors, wait relationships, process
states, thread counts and the ASID table. There is no second scheduler lock,
so there is no lock order.

- The lock is only ever taken with all exceptions masked. Scheduler entry
  points reject unmasked callers, and the tick handler runs masked. A tick can
  therefore never interrupt its own core while that core holds the lock.
- No task is eligible while executing. On yield, block, exit or tick the task
  hands the lock to its own dispatcher through the register switch. Only after
  the context and stack pointer are saved does the dispatcher clear ownership,
  drop the user address space and release the lock. Another core can then
  claim the task, or reclaim its stack and address space.
- Idle dispatchers sleep in WFE with interrupts masked and no timer armed.
  Every publication of runnable work (create, wake, preempt, kill) is followed
  by SEV; the event register covers the window between the check and WFE.
- File-handle cleanup, table construction and page freeing happen outside the
  lock, on core 0.

The scheduler does not protect application data. Kernel tasks sharing data
need their own synchronization and must not yield while holding a lock that
another task on the same core needs.

## Processes

`include/vantage/kernel/process.h`. A process is created from a flat program
image in kernel memory:

```c
ProcessId app = process_create(image, bytes, TASK_CORE0);
/* ... task_run() ... */
int status;
process_reap(app, &status);   /* or process_wait() from a kernel task */
```

| Step | What happens |
|---|---|
| `process_create` | Validates the image header; takes a slot and an ASID; builds the tables; copies text, constants and data into newly allocated pages; cleans and invalidates the instruction cache; maps the first thread's stack; makes the thread runnable. Any failure releases everything again. |
| `process_thread` | Maps another 16 KiB stack and starts a thread at `entry(argument)`. At most four threads. |
| Exit | `SYS_TASK_EXIT(status)` from any thread ends the whole process. A synchronous fault, a forbidden syscall buffer or `process_kill` ends it too. The first request fixes the status. |
| Stopping threads | The calling thread exits at once. Other threads never return to EL0: a runnable thread is retired by the next dispatcher that sees it, and a thread running on another core exits at its next kernel entry, at the latest its next tick. |
| Zombie | When the last thread is off its core and stack the process is `PROCESS_EXITED` and keeps its slot, memory and status. |
| `process_reap` | Nonblocking; `TASK_PENDING` until the process has exited. Reaps the thread slots, releases the process's file handles, flushes its ASID on every core, frees every page and table, releases the ASID and invalidates the ID. |

Creation, thread creation and reaping use the page allocator and therefore
run only on core 0, from boot or a core-0-pinned kernel task, like the
allocator itself. `process_kill`, `process_info` and `process_current` work on
any core. There are eight process slots.

Fault statuses (`enum ProcessFault`): data abort −1001, instruction abort
−1002, undefined or privileged instruction −1003, forbidden syscall buffer
−1004, any other synchronous exception −1005. A fault ends only the offending
process; the kernel and other processes continue.

File handles opened through system calls belong to the calling process, not
to the thread. They are released when a core-0-pinned thread exits the
process, and again at reap.

### Program images

An image starts with `ProcessImage`: magic, entry address, and the sizes of
text, constants, data and bss. Text and constants are page multiples. The
terminal is the only program. `user/` is compiled and linked on its own with
`user/program.ld` at user addresses, including its own copy of the
freestanding memory routines; the build converts it to a flat binary and
embeds it in the kernel's read-only data (`kernel/boot/terminal_image.s`).
The program is never linked against the kernel, so an undefined symbol fails
the build, and the kernel image contains no application or syscall-wrapper
code. `user/lib/start.s` calls `user_task` and exits the process if it
returns.

## Address spaces

The MMU uses one 39-bit TTBR0 range (`T0SZ = 25`) with 8-bit ASIDs; TTBR1 is
disabled.

| Virtual range | Contents | EL1 | EL0 |
|---|---|---|---|
| `0x0000_0000`–`0x001f_ffff` | Kernel text and constants (identity) | Read, execute | None |
| Rest of the first 4 GiB | Kernel data, RAM, framebuffer, devices (identity) | Read, write | None |
| `0x1_0000_0000` + image | Program text | — | Read, execute |
| … | Program constants | — | Read |
| … | Program data and bss | — | Read, write |
| Below `0x1_4000_0000` | Thread stacks, 16 KiB every 64 KiB, unmapped between | — | Read, write |

- Kernel blocks are global, never accessible from EL0 (`AP[1] = 0`) and never
  executable from EL0 (`UXN`). Only the kernel text block is executable at EL1
  and it is read-only; everything else is `PXN`. The kernel tables are built
  once and stay immutable.
- Each process has its own level-1 table. Its first four entries are the
  kernel's level-1 entries, so kernel mappings are identical under every
  root; the fifth points to the process's own level-2 table for the user
  gigabyte. User pages are non-global 4 KiB pages, `PXN`, and writable or
  executable but never both. User memory outside these regions is unmapped,
  including a gap below every stack.
- Process memory is not shared: pages are allocated per process and freed
  only at reap. Tables only gain entries while the process lives.

**Switching.** Before the dispatcher switches to a task it installs that
task's root and ASID in `TTBR0_EL1`, or the kernel-only root with ASID 0 for a
kernel task. When the task returns to the dispatcher, the dispatcher installs
the kernel-only root again before it releases the lock. A core therefore has a
process's tables installed only while one of its threads is current there, and
never after that thread can be observed as exited. No TLB flush is needed on a
switch.

**ASIDs.** ASID 0 is the kernel-only root. A process holds one of the ASIDs
1–255 from creation until reap. Allocation is next-fit, so the values wrap
around; an ASID is handed out again only after `TLBI ASIDE1IS` has removed its
translations on every core at teardown, before the pages are freed. Stale
translations for a reused ASID therefore cannot exist. Nothing is ever
unmapped from a live process, so no other invalidation is required.

**System-call buffers.** `include/vantage/kernel/user_access.h`. The kernel
never dereferences a user pointer. A buffer is resolved through the calling
process's tables: the complete range must be mapped for EL0 with the needed
permission. Data is then copied between the kernel stack and the kernel's own
mapping of those pages; services only see the kernel copy. Calls that return
data check that the destination is writable before they have any effect.
A buffer the process may not access terminates the process (−1004) before the
call has any effect. Misaligned buffers and invalid field values are ordinary
errors, as before. Callers that are not process threads have no address space
and are refused.

## Device and service ownership

Existing non-reentrant services keep their contracts: the page allocator and
heap, storage and SD, the property mailbox, display and renderer, input, and
the worker reservation of `parallel_run`. They require core 0 at EL1 with all
exceptions masked, from boot or an immutable core-0-pinned task. Because the
kernel is not preemptible, a tick cannot re-enter them or interleave two
callers on core 0: a system call runs to completion or to an explicit yield
before another core-0 task is dispatched.

The terminal process is pinned to core 0. File, input and display system calls
accept boot or core-0-pinned tasks only, not movable tasks that happen to run
there; file calls report `FILE_UNAVAILABLE` otherwise. Presentation and
frame-wait system calls still yield after the renderer has finished.

Secondary workers handle synchronous batches and scheduler polling. During a
task session `parallel_run` first tries to reserve all idle workers under the
scheduler lock; reservation excludes task dispatch until the batch completes.
If any secondary is running a task, the batch runs as one serial slice on
core 0. Batch callbacks run in dispatcher context with interrupts masked and
no timer armed; they must not yield, block or change scheduler state.

## System calls

Existing numbers and payload layouts are unchanged; no system call was added.

| Call | ID | Result |
|---|---|---|
| `sys_task_id()` | 16 | Current thread's task ID |
| `sys_task_yield()` | 17 | One after resuming |
| `sys_task_exit(status)` | 18 | Ends the calling process; does not return |

Creating tasks, threads and processes, joining, waiting and killing are kernel
APIs. The desktop simulator has no scheduler, privilege levels or address
spaces: it runs the same application and dispatcher in one host process.

## Verification

```sh
cmake -S . -B /path/to/host
cmake --build /path/to/host --config Release
ctest --test-dir /path/to/host -C Release -L scheduler --output-on-failure
for suite in interrupts processes preemption terminal tasks task_smp files; do
  python -B tests/arm/$suite.py /path/to/fresh/kernel8.elf -v
done
```

**Host tests** run the production scheduler, process layer and translation
table code on four OS threads; fibers or `ucontext` replace the register
switch, and doubles replace the page allocator, table maintenance and EL0.

- `scheduler_four_core`: simultaneous entry, migration at yields, cross-core
  join/reap, capacity, slot reuse and raster-batch ownership for kernel tasks.
- `scheduler_processes`: lifecycle and complete memory release; 600 address
  spaces over 255 ASIDs with one resident process across the wrap; rollback
  for every allocation failure; rejected images; table layout and permission
  bits; range checks; forbidden buffers; threads, exit, kill and wait; no core
  has a root installed when its ASID is flushed.
- `scheduler_preemption`: a simulated per-core tick taken at explicit points:
  non-yielding processes sharing a core, migration of a preempted process,
  termination of a looping process on another core, kernel regions, and the
  boot probe.

**ARM models** execute the linked `kernel8.elf`.

- `tasks.py`, `task_smp.py` and the kernel-level tests in `files.py` use the
  scripted machine: privileged instructions and devices are mocked and there
  is no MMU. They cover kernel tasks only.
- `interrupts.py`, `processes.py`, `preemption.py`, `terminal.py` and the EL0
  tests in `files.py` use the CPU model in `tests/arm/cpu.py`. Unicorn executes
  EL0 and EL1 code, the kernel's translation tables with their permissions,
  system registers and exception returns natively. The model supplies
  exception entry, four CPU contexts over shared RAM, a GIC CPU interface and a
  timer whose slices are counted in executed instructions. Unicorn keeps
  running already-translated code until the kernel invalidates the instruction
  cache, so a missing invalidation is visible.

They check: vector routing; the frame contents and full register restoration
for an interrupt at every instruction of a test routine at EL0 and at EL1; GIC
and timer programming per core; that a looping EL0 process is preempted and
killed within a slice; register preservation of a preempted process that
migrates; six processes with different data at one address under preemption;
a tick pending during a system call; kernel tasks preempted only inside
regions on all four cores; EL0 access to kernel memory, devices, other
processes' pages, privileged instructions and the timer; forbidden syscall
buffers; teardown ordering (ASID flush before freeing) and memory accounting;
and the embedded terminal running as a process, including file editing.

These models do not validate firmware, secure/non-secure GIC state, real
interrupt delivery or latency, timer frequency, TLB and cache hardware,
broadcast invalidation between cores, or timing.

## Physical acceptance checklist

Keep each result tied to the board/revision, firmware/configuration, commit,
build options, kernel SHA-256 and full UART log. Record `config.txt`; the
kernel expects the default GIC handoff.

- [ ] Repeated cold boots show all secondary entries, the parallel checksum
      test, the task self-test, the four-core overlap test and
      `[OK] Timer ticks preempted kernel tasks on cores 0, 1, 2 and 3`.
- [ ] A build whose firmware configuration disables the GIC halts with
      `[ERR] GIC timer interrupt unavailable; boot halted`.
- [ ] Measure the tick: a preemptible kernel region is preempted about every
      10 ms on each core, and `CNTFRQ_EL0` matches the board.
- [ ] The terminal runs as a process: boot to the prompt, sustained typing,
      touch, rendering and file editing behave as before.
- [ ] A disposable image whose program loops without system calls does not
      stop other core-0 tasks, and is terminated by `process_kill`.
- [ ] Disposable programs that read or write kernel memory, device registers
      and unmapped addresses, execute data, or pass forbidden syscall buffers
      are terminated with the documented status while the system continues.
- [ ] Movable processes keep integer, SIMD and flag state while being
      preempted and resumed on other cores under load.
- [ ] Create and reap more than 255 processes; memory statistics return to
      their baseline, and reused ASIDs never expose stale translations.
- [ ] Two processes started from different images show their own code after
      one replaces the other in the same physical pages.
- [ ] Cross-core join/exit races never resume a task twice or reuse an active
      stack; cyclic waits and a second waiter are rejected.
- [ ] Idle workers wake reliably around publication and WFE/SEV races, and
      idle cores take no timer interrupts.
- [ ] Core-0 touch/display/file behaviour stays correct under compute load on
      the other cores; remote and movable tasks cannot use those services.
- [ ] Record pixel correctness and frame times before making any throughput
      or latency claim. Validate SD separately using STORAGE.md.
