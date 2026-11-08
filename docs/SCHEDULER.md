# Four-core cooperative task scheduling

## Status and scope

The scheduler can execute trusted tasks simultaneously on Pi 4 cores 0–3.
Each core has a private dispatcher context and an exclusive current-task slot.
One locked, bounded task table provides round-robin selection by immutable
affinity. There are 32 task slots with 16 KiB privileged stacks, diagnostic stack
guards, non-wrapping task IDs, cross-core joins and safe delayed stack reclamation.

**Physical Pi acceptance is pending.** Host threads and linked ARM models check
software behavior, not firmware handoff, real cache coherence or hardware speed.
This change does not enable the separate SD hardware-validation boot gate.

Scheduling remains cooperative, with masked exceptions. There is no timer
preemption, priority policy, task cancellation, process loader or independent
address space. A task that never yields occupies its core indefinitely. Existing
shared user memory is unchanged. A single function is not automatically split
into four parallel functions: callers must submit independent work.

## Creating work

Initialize after memory/MMU setup, secondary startup and `parallel_init`.
Initialization is one-shot. The existing `task_create` pins work to core 0 for
compatibility with device services. New compute work can use explicit affinity:

```c
#include "includes/task.h"

static void compute(void* argument) {
    unsigned int shard = (unsigned int)(uintptr_t)argument;
    /* Operate on this shard's separately owned data. Yield between bounded
     * chunks if other tasks should get time on the same core. */
    (void)shard;
    task_yield();
    task_exit(0);
}

/* Called from the core-0 boot coordinator after task_init. */
static int run_shards(void) {
    TaskId ids[TASK_CORES];
    for (unsigned int i = 0; i < TASK_CORES; i++) {
        ids[i] = task_create_affinity(compute, (void*)(uintptr_t)i, 1U << i);
        if (!ids[i]) return 0; /* A production caller must handle partial setup. */
    }
    if (task_run() != TASK_OK) return 0;
    for (unsigned int i = 0; i < TASK_CORES; i++) {
        int status;
        if (task_reap(ids[i], &status) != TASK_OK || status) return 0;
    }
    return 1;
}
```

Use `TASK_ALL_CORES` instead of `1U << i` for movable work. Any allowed idle core
may claim it; after yielding/blocking it may resume on a different allowed core.
Do not cache the core ID across those calls. Pin tasks that depend on core-local
state. Affinity zero, out-of-range bits and unavailable cores are rejected.
Without the initialized four-core worker facility, only affinity 1 is available.

Task creation, join, reap, state and info calls are masked-EL1 kernel APIs. The
existing EL0 ID/yield/exit syscalls work on all participating cores; creating
tasks and joining them remain kernel-only. Entries and arguments are trusted.
An argument or result buffer must outlive every task that accesses it.

## Synchronization and lifetime

Acquire/release locking protects task states, slot allocation, wait relationships,
dispatch counters and selection cursors. No task is eligible while executing.
On yield, block or exit the task hands the lock to its own dispatcher through
the context switch. Only after all registers and the stack pointer have been
saved does that dispatcher clear ownership and release the lock. A different
core can then claim the runnable task or reclaim its exited stack.

The AAPCS64 cooperative switch preserves callee-saved integer/FP state, FPCR/FPSR,
ELR_EL1/SPSR_EL1/SP_EL0. This supports migration at a voluntary call boundary,
including a yield inside a pending EL0 SVC. It is not a complete interrupt frame.
All cores use the existing immutable shared mappings, so this implementation
does not change process page tables or introduce address-space switches.

Join reserves a target for one waiter and rejects self/cyclic waits under the
same lock. Exit wakes its waiter only after switching off the exiting stack.
Reap is nonblocking, rejects reserved/live targets, and makes a slot reusable
only after context disposal. An exited task otherwise continues to consume a
slot. `task_run` returns only when all cores are globally quiescent; blocked-only
state returns `TASK_PENDING`. There is no forced recovery of a stalled core/task.

The scheduler does not protect arbitrary shared application data. Use disjoint
buffers or explicit acquire/release synchronization. Do not yield while holding
a lock needed by another task on the same core. The scheduler's own lock is not
available to application code and is not interrupt-safe.

## Device and renderer ownership

The built-in application remains pinned to core 0. Existing file, input and
display syscalls accept boot or immutable core-0-pinned tasks, not movable tasks
temporarily executing there. Raw file APIs enforce the same restriction on Pi.
File-handle cleanup therefore runs only on core 0; other cores cannot open
handles through these interfaces. Existing raw SD, mailbox and allocator ownership
contracts still apply. Trusted kernel tasks must honor each driver's contract.

Secondary workers handle bounded synchronous batches and scheduler polling.
During a task session, `parallel_run` first attempts to reserve all idle workers
under the scheduler lock. Reservation excludes task dispatch until every batch
slice completes, preserving existing four-way rendering when workers are free.
If any secondary is executing a task, it executes one serial slice on core 0
instead of waiting for that task to yield. After release, workers resume polling
tasks. Callbacks must not yield, block or mutate scheduler state. This ownership
policy is not a claim that rendering becomes faster with multicore scheduling.

## Verification

```sh
cmake -S kernel/sched -B /path/to/scheduler-tests
cmake --build /path/to/scheduler-tests --config Release
ctest --test-dir /path/to/scheduler-tests -C Release --output-on-failure
python -B tests/arm/tasks.py /path/to/fresh/kernel8.elf -v
python -B tests/arm/task_smp.py /path/to/fresh/kernel8.elf -v
```

The native test runs the production scheduler on four host OS threads. Windows
fibers or POSIX ucontext replace the ARM register switch. It checks simultaneous
entry on all cores, forced live migration, cross-core join/reap exclusivity,
concurrent creation at full capacity, repeated slot reuse, and raster ownership.
The Windows fiber stacks are host-managed; ARM tests exercise actual task-stack
and register-switch assembly. CI configures the POSIX test with UBSan; this is
separate from the address/undefined sanitizers on storage and driver models.

The linked ARM model interleaves four register/CPU contexts over shared modeled
RAM, explicitly modeling per-CPU exclusive reservations. Tests execute scheduler
C and context assembly, check that no task is owned by two cores, verify integer,
FP and exception-return preservation, and exercise remote joins, cycles, service
rejection and the boot overlap probe. Scripted task bodies are not a hardware
workload or a benchmark.

## Physical acceptance checklist

Keep each result tied to the board/revision, firmware/configuration, commit,
build options, kernel SHA-256 and full UART log:

- [ ] Repeated cold boots show all secondary entries, the existing parallel
      checksum test, the cooperative test and the four-core task overlap test.
- [ ] Each core makes sustained progress with its own pinned task; then run more
      tasks than cores and verify repeated yield/exit/reap and slot reuse.
- [ ] Movable tasks retain stack locals, callee-saved FP/integer registers and
      pending exception-return state while visiting multiple allowed cores.
- [ ] Cross-core join/exit races never resume a task twice or reuse an active
      stack; cyclic waits and a second waiter are rejected.
- [ ] Idle workers wake reliably around publication and WFE/SEV races.
- [ ] Core-0 touch/display/file behavior remains correct under compute load;
      remote and movable tasks cannot change those services through syscalls.
- [ ] Record pixel correctness and frame-time comparisons before making any
      throughput or latency claims. Validate SD separately using STORAGE.md.

Timer/GIC interrupt setup and preemptive scheduling are a subsequent change,
requiring full interrupt context handling and interrupt-safe service ownership.
