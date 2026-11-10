#pragma once
#include <vantage/kernel/task_port.h>
#include <vantage/kernel/process.h>
#include <vantage/arch/address_space.h>

#ifdef _MSC_VER
#define TASK_ALIGN __declspec(align(16))
#else
#define TASK_ALIGN __attribute__((aligned(16)))
#endif

typedef struct Process Process;

typedef struct {
    TaskContext context;
    TaskEntry entry;
    void* argument;
    TaskId id, generation, waiting_for, waiter;
    enum TaskState state;
    int status;
    uint32_t affinity, core;
    uint64_t dispatches, preemptions;
    Process* process; /* NULL for kernel tasks; immutable while the slot is used. */
    uint64_t stack_alignment;
    TASK_ALIGN unsigned char stack[TASK_STACK_BYTES];
} Task;

typedef struct {
    TaskId task;
    uint64_t entry, argument, stack;
} ProcessThread;

/* Core 0 alone creates, extends and tears down a process. Fields read by other
 * cores (state, terminating, status, live, root) change under the scheduler
 * lock; the address space only gains mappings while the process is live. */
struct Process {
    ProcessId id;
    uint64_t generation;
    enum ProcessState state;
    int status;
    uint32_t terminating;
    uint32_t threads, live;
    uint32_t asid;
    uint64_t root;
    AddressSpace space;
    ProcessThread thread[PROCESS_THREADS];
};

/* Scheduler internals shared with the process layer. */
void sched_lock(void);
void sched_unlock(void);
/* The calling core's running task; NULL in boot/dispatcher context. */
Task* sched_running(void);
/* Lock held. */
Task* sched_lookup(TaskId id);
void sched_release(Task* task);
/* A process thread is counted in process->live under the same lock that makes
 * it runnable, and is refused once the process is terminating. */
TaskId sched_spawn(TaskEntry entry, void* argument, uint32_t affinity, Process* process);

/* Lock held. Live or exited processes only. */
Process* process_lookup(ProcessId id);
/* Lock held; the thread is exited and no longer on any core or stack. */
void process_thread_exited(Process* process);
