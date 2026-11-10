#pragma once
#include <vantage/base.h>

/* Preemptive tasks on four cores. Kernel tasks are trusted EL1 code that runs
 * with interrupts masked and is only preempted inside an explicit preemptible
 * region; process threads run at EL0 and are preempted by the timer tick.
 * Scheduler calls require EL1 with all exceptions masked. */
#define TASK_CORES 4U
#define TASK_CORE0 1U
#define TASK_ALL_CORES 15U
#define TASK_LIMIT 32U
#define TASK_STACK_BYTES 16384U
/* Longest a task keeps its core while preemptible. */
#define TASK_SLICE_US 10000U
typedef uint64_t TaskId;
typedef void (*TaskEntry)(void* argument);

enum TaskResult {
    TASK_OK = 1,
    TASK_INVALID = 0,
    TASK_PENDING = -1,
    TASK_BUSY = -2,
    TASK_DEADLOCK = -3
};

enum TaskState {
    TASK_FREE, TASK_RUNNABLE, TASK_RUNNING, TASK_BLOCKED, TASK_EXITED
};

int task_init(void);
/* Compatibility/default for tasks using core-0-owned device services. */
TaskId task_create(TaskEntry entry, void* argument);
/* Immutable affinity; movable tasks may resume on a different allowed core
 * after yielding. Shared application data requires its own synchronization. */
TaskId task_create_affinity(TaskEntry entry, void* argument, uint32_t affinity);
TaskId task_current(void);
unsigned int task_core(void);
/* True only for boot/core-0-pinned tasks. Does not transfer driver ownership. */
int task_services_allowed(void);
int task_yield(void);
/* Opens an interrupt-enabled region in the calling kernel task: the tick may
 * preempt it at any instruction and it may resume on another allowed core.
 * Inside the region the task must hold no kernel resource and call nothing
 * but task_preempt_end; scheduler and device services reject such callers. */
int task_preempt_begin(void);
void task_preempt_end(void);
void task_exit(int status);
/* Join is for running tasks; reap is nonblocking and also works from boot. */
int task_join(TaskId id, int* status);
int task_reap(TaskId id, int* status);
int task_state(TaskId id, enum TaskState* state);
typedef struct {
    TaskId id;
    uint64_t dispatches;
    uint32_t affinity, core;
    enum TaskState state;
    uint64_t preemptions; /* Times the tick took the core away. */
} TaskInfo;
/* core is TASK_CORES when the task is not executing. */
int task_info(TaskId id, TaskInfo* info);
/* Core 0 coordinates all available cores until global quiescence. Returns
 * PENDING for blocked tasks. A kernel task that neither yields nor opens a
 * preemptible region keeps its core. */
int task_run(void);
int task_self_test(void);
int task_multicore_self_test(void);
/* Every core must take timer ticks inside a preemptible kernel region. */
int task_preemption_self_test(void);
