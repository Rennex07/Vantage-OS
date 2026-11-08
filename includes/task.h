#pragma once
#include "std.h"

/* Trusted cooperative EL1 tasks. Scheduler calls require masked exceptions. */
#define TASK_CORES 4U
#define TASK_CORE0 1U
#define TASK_ALL_CORES 15U
#define TASK_LIMIT 32U
#define TASK_STACK_BYTES 16384U
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
} TaskInfo;
/* core is TASK_CORES when the task is not executing. */
int task_info(TaskId id, TaskInfo* info);
/* Core 0 coordinates all available cores until global quiescence. Returns
 * PENDING for blocked tasks. No timeout/preemption of non-yielding tasks. */
int task_run(void);
int task_self_test(void);
int task_multicore_self_test(void);
