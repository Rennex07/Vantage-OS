#include "../../includes/task_internal.h"
#include "../../includes/files_internal.h"
#include "../../includes/parallel_internal.h"

#define ID_BITS 6U
#define ID_MASK 63U
#define GUARD_BYTES 64U
#define GUARD_VALUE 0xa7U

typedef char task_ids_must_fit[(TASK_LIMIT < ID_MASK) ? 1 : -1];
typedef char task_cores_must_match[(TASK_CORES == PARALLEL_CORES) ? 1 : -1];

#ifdef _MSC_VER
#define TASK_ALIGN __declspec(align(16))
#else
#define TASK_ALIGN __attribute__((aligned(16)))
#endif

typedef struct {
    TaskContext context;
    TaskEntry entry;
    void* argument;
    TaskId id, generation, waiting_for, waiter;
    enum TaskState state;
    int status;
    uint32_t affinity, core;
    uint64_t dispatches;
    uint64_t stack_alignment;
    TASK_ALIGN unsigned char stack[TASK_STACK_BYTES];
} Task;

static Task tasks[TASK_LIMIT];
static TaskContext dispatcher[TASK_CORES];
/* Each current slot is written by its core under the lock. Other cores may
 * inspect slots only under the lock, when reserving idle workers for a batch. */
static Task* current[TASK_CORES];
/* The table, cursors and dispatching are protected by scheduler_lock. */
static unsigned int cursor[TASK_CORES];
static uint32_t scheduler_lock, initialized;
static int dispatching, workers_reserved;

static int owned(void) { return task_port_owned() && parallel_load(&initialized); }
static void lock(void) { task_port_lock(&scheduler_lock); }
static void unlock(void) { task_port_unlock(&scheduler_lock); }

static Task* lookup(TaskId id) {
    unsigned int slot = (unsigned int)(id & ID_MASK);
    if (!slot || slot > TASK_LIMIT) return NULL;
    Task* task = &tasks[slot - 1];
    return task->state != TASK_FREE && task->id == id ? task : NULL;
}

static void check_stack(const Task* task) {
    uintptr_t bottom = (uintptr_t)task->stack + GUARD_BYTES;
    uintptr_t top = (uintptr_t)task->stack + TASK_STACK_BYTES;
    for (unsigned int i = 0; i < GUARD_BYTES; i++)
        if (task->stack[i] != GUARD_VALUE)
            task_port_halt("[ERR] Task stack guard damaged; halted\n");
    if (task->context.sp < bottom || task->context.sp > top || (task->context.sp & 15U))
        task_port_halt("[ERR] Task stack pointer invalid; halted\n");
}

static void bootstrap(void) {
    Task* task = current[task_port_core()];
    task->entry(task->argument);
    task_exit(0);
}

/* Called by exactly one dispatcher per core. A yielding/exiting/blocking task
 * hands the lock to its dispatcher across the context switch. Until its entire
 * context is saved, no other core may dispatch, join or reap that task. */
static uint32_t dispatch_one(unsigned int core) {
    lock();
    Task* next = NULL;
    if (dispatching && (!core || !workers_reserved)) {
        for (unsigned int offset = 1; offset <= TASK_LIMIT; offset++) {
            unsigned int slot = (cursor[core] + offset) % TASK_LIMIT;
            if (tasks[slot].state == TASK_RUNNABLE && (tasks[slot].affinity & (1U << core))) {
                next = &tasks[slot];
                cursor[core] = slot;
                break;
            }
        }
    }
    if (!next) { unlock(); return 0; }
    check_stack(next);
    current[core] = next;
    next->state = TASK_RUNNING;
    next->core = core;
    if (next->dispatches != ~(uint64_t)0) next->dispatches++;
    unlock();
    task_context_switch(&dispatcher[core], &next->context);
    /* The resumed dispatcher owns the lock handed over by this core's task. */
    check_stack(next);
    next->core = TASK_CORES;
    current[core] = NULL;
    if (next->state == TASK_EXITED) {
        Task* waiter = lookup(next->waiter);
        if (waiter && waiter->state == TASK_BLOCKED && waiter->waiting_for == next->id)
            waiter->state = TASK_RUNNABLE;
    }
    unlock();
    parallel_wake();
    return 1;
}

static int reserve_workers(int claim) {
    lock();
    if (claim) {
        for (unsigned int core = 1; core < TASK_CORES; core++) {
            if (current[core]) { unlock(); return 0; }
        }
    }
    workers_reserved = claim != 0;
    unlock();
    if (!claim) parallel_wake();
    return 1;
}

int task_init(void) {
    if (!task_port_owned() || task_port_core() || parallel_load(&initialized)) return TASK_INVALID;
    if (!parallel_task_install(dispatch_one, reserve_workers)) return TASK_INVALID;
    for (unsigned int core = 0; core < TASK_CORES; core++) cursor[core] = TASK_LIMIT - 1;
    parallel_store(&initialized, 1);
    return TASK_OK;
}

TaskId task_create_affinity(TaskEntry entry, void* argument, uint32_t affinity) {
    if (!owned() || !entry || !affinity || (affinity & ~parallel_task_mask())) return 0;
    lock();
    TaskId id = 0;
    for (unsigned int i = 0; i < TASK_LIMIT; i++) {
        Task* task = &tasks[i];
        if (task->state != TASK_FREE || task->generation == (~(TaskId)0 >> ID_BITS)) continue;
        task->generation++;
        task->id = (task->generation << ID_BITS) | (i + 1U);
        task->entry = entry;
        task->argument = argument;
        task->waiting_for = task->waiter = 0;
        task->status = 0;
        task->affinity = affinity;
        task->core = TASK_CORES;
        task->dispatches = 0;
        for (unsigned int j = 0; j < GUARD_BYTES; j++) task->stack[j] = GUARD_VALUE;
        task_context_init(&task->context, task->stack + GUARD_BYTES,
                          TASK_STACK_BYTES - GUARD_BYTES, bootstrap);
        task->state = TASK_RUNNABLE;
        id = task->id;
        break;
    }
    unlock();
    if (id) parallel_wake();
    return id;
}

TaskId task_create(TaskEntry entry, void* argument) {
    return task_create_affinity(entry, argument, TASK_CORE0);
}

unsigned int task_core(void) { return task_port_core(); }

TaskId task_current(void) {
    if (!owned()) return 0;
    Task* task = current[task_port_core()];
    return task ? task->id : 0;
}

int task_services_allowed(void) {
    if (!task_port_owned() || task_port_core()) return 0;
    Task* task = current[0];
    return !task || task->affinity == TASK_CORE0;
}

int task_yield(void) {
    if (!owned()) return TASK_INVALID;
    unsigned int core = task_port_core();
    Task* task = current[core];
    if (!task) return TASK_INVALID;
    lock();
    task->state = TASK_RUNNABLE;
    task_context_switch(&task->context, &dispatcher[core]);
    return TASK_OK;
}

void task_exit(int status) {
    if (!owned() || !current[task_port_core()])
        task_port_halt("[ERR] Task exit outside scheduler; halted\n");
    unsigned int core = task_port_core();
    Task* task = current[core];
    /* Only immutable core-0-affinity tasks may own file handles. */
    if (task->affinity == TASK_CORE0) files_release(task->id);
    lock();
    task->status = status;
    task->state = TASK_EXITED;
    task_context_switch(&task->context, &dispatcher[core]);
    task_port_halt("[ERR] Exited task resumed; halted\n");
}

static int reap(Task* task, int* status) {
    if (status) *status = task->status;
    task_context_dispose(&task->context);
    task->state = TASK_FREE;
    return TASK_OK;
}

int task_reap(TaskId id, int* status) {
    if (!owned()) return TASK_INVALID;
    lock();
    Task* task = lookup(id);
    int result = TASK_INVALID;
    if (task && task != current[task_port_core()]) {
        result = task->waiter ? TASK_BUSY : task->state != TASK_EXITED ? TASK_PENDING : reap(task, status);
    }
    unlock();
    return result;
}

int task_join(TaskId id, int* status) {
    if (!owned()) return TASK_INVALID;
    unsigned int core = task_port_core();
    Task* self = current[core];
    if (!self) return TASK_INVALID;
    lock();
    Task* task = lookup(id);
    if (!task) { unlock(); return TASK_INVALID; }
    if (task == self) { unlock(); return TASK_DEADLOCK; }
    if (task->waiter) { unlock(); return TASK_BUSY; }
    Task* dependency = task;
    for (unsigned int i = 0; dependency && i < TASK_LIMIT; i++) {
        if (dependency == self) { unlock(); return TASK_DEADLOCK; }
        dependency = lookup(dependency->waiting_for);
    }
    if (task->state != TASK_EXITED) {
        task->waiter = self->id;
        self->waiting_for = id;
        self->state = TASK_BLOCKED;
        task_context_switch(&self->context, &dispatcher[core]);
        /* May have resumed on a different core. Never reuse the old dispatcher. */
        lock();
        self->waiting_for = 0;
    }
    int result = reap(task, status);
    unlock();
    return result;
}

int task_info(TaskId id, TaskInfo* info) {
    if (!owned() || !info) return TASK_INVALID;
    lock();
    Task* task = lookup(id);
    if (task) {
        info->id = task->id;
        info->dispatches = task->dispatches;
        info->affinity = task->affinity;
        info->core = task->core;
        info->state = task->state;
    }
    unlock();
    return task ? TASK_OK : TASK_INVALID;
}

int task_state(TaskId id, enum TaskState* state) {
    if (!state) return TASK_INVALID;
    TaskInfo info;
    int result = task_info(id, &info);
    if (result == TASK_OK) *state = info.state;
    return result;
}

int task_run(void) {
    if (!owned() || task_port_core() || current[0]) return TASK_INVALID;
    lock();
    if (dispatching) { unlock(); return TASK_INVALID; }
    dispatching = 1;
    unlock();
    if (!parallel_task_start()) {
        lock(); dispatching = 0; unlock();
        return TASK_INVALID;
    }
    for (;;) {
        if (dispatch_one(0)) continue;
        lock();
        int live = 0, blocked = 0;
        for (unsigned int i = 0; i < TASK_LIMIT; i++) {
            if (tasks[i].state == TASK_RUNNING || tasks[i].state == TASK_RUNNABLE) live = 1;
            if (tasks[i].state == TASK_BLOCKED) blocked = 1;
        }
        if (!live) {
            dispatching = 0;
            unlock();
            parallel_task_stop();
            return blocked ? TASK_PENDING : TASK_OK;
        }
        unlock();
        /* Publication -> SEV pairs with WFE, including check/sleep races. */
        parallel_idle();
    }
}
