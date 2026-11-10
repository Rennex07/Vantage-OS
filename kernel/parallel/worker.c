#include <vantage/kernel/parallel_port.h>

#if defined(_MSC_VER)
#define CACHE_ALIGNED __declspec(align(64))
#else
#define CACHE_ALIGNED __attribute__((aligned(64)))
#endif

typedef struct CACHE_ALIGNED {
    uint32_t submitted;
    uint32_t completed;
    uint32_t online;
    uint32_t reserved;
    parallel_job job;
    void* context;
    unsigned char padding[64 - 4 * sizeof(uint32_t) - sizeof(parallel_job) - sizeof(void*)];
} Worker;

static Worker workers[PARALLEL_CORES];
/* These fields belong exclusively to core 0; worker threads never read them. */
static uint32_t enabled, active, generation;
static uint32_t tasks_active;
static uint32_t (*task_poll)(unsigned int core);
static int (*task_reserve)(int claim);

#define WAIT_US 1000000u
#define WAIT_LIMIT 0x4000000u

void parallel_halt(const char* message) {
    parallel_port_halt(message);
    for (;;) parallel_idle();
}

static uint32_t wait_for(const uint32_t* value, uint32_t expected) {
    uint32_t start = parallel_time_us();
    uint32_t remaining = WAIT_LIMIT;
    while (parallel_load(value) != expected) {
        if (parallel_time_us() - start >= WAIT_US || !--remaining) return 0;
        /* Poll here so a missing wake event cannot defeat the deadline. */
    }
    return 1;
}

void parallel_worker_online(unsigned int core) {
    if (core > 0 && core < PARALLEL_CORES && parallel_core_id() == core) {
        parallel_store(&workers[core].online, 1);
        parallel_wake();
    }
}

uint32_t parallel_worker_poll(unsigned int core) {
    if (!core || core >= PARALLEL_CORES || parallel_core_id() != core) return 0;
    Worker* worker = &workers[core];
    uint32_t next = parallel_load(&worker->submitted);
    if (next == parallel_load(&worker->completed)) {
        if (parallel_load(&tasks_active)) return task_poll(core);
        return 0;
    }
    /* Acquire submitted before loading the callback/context. The coordinator
     * does not overwrite this slot until it acquires completed. */
    worker->job(core, PARALLEL_CORES, worker->context);
    parallel_completion_barrier();
    parallel_store(&worker->completed, next);
    parallel_wake();
    return 1;
}

void parallel_worker_entry(unsigned int core) {
    parallel_worker_online(core);
    for (;;) {
        if (!parallel_worker_poll(core)) parallel_idle();
    }
}

uint32_t parallel_init(void) {
    if (parallel_core_id() != 0 || enabled || active || task_poll) return 0;
    for (unsigned int core = 1; core < PARALLEL_CORES; core++) {
        if (!wait_for(&workers[core].online, 1)) return 0;
    }
    enabled = 1;
    return 1;
}

uint32_t parallel_run(parallel_job job, void* context) {
    if (parallel_core_id() != 0 || !job || active) return 0;
    active = 1;
    int task_mode = parallel_load(&tasks_active) != 0;
    int reserved = enabled && task_mode && task_reserve(1);
    if (!enabled || (task_mode && !reserved)) {
        job(0, 1, context);
    } else {
        uint32_t next = ++generation;
        for (unsigned int core = 1; core < PARALLEL_CORES; core++) {
            workers[core].job = job;
            workers[core].context = context;
            parallel_store(&workers[core].submitted, next);
        }
        parallel_wake();
        job(0, PARALLEL_CORES, context);
        parallel_completion_barrier();
        for (unsigned int core = 1; core < PARALLEL_CORES; core++) {
            if (!wait_for(&workers[core].completed, next)) {
                static const char* const faults[PARALLEL_CORES] = {
                    "", "[ERR] Parallel core 1 job timed out; system halted\n",
                    "[ERR] Parallel core 2 job timed out; system halted\n",
                    "[ERR] Parallel core 3 job timed out; system halted\n"
                };
                parallel_halt(faults[core]);
            }
        }
    }
    if (reserved) (void)task_reserve(0);
    active = 0;
    return 1;
}

int parallel_task_install(uint32_t (*poll)(unsigned int core), int (*reserve)(int claim)) {
    if (parallel_core_id() || !poll || !reserve || task_poll || active) return 0;
    task_poll = poll;
    task_reserve = reserve;
    return 1;
}

uint32_t parallel_task_mask(void) { return enabled ? (1U << PARALLEL_CORES) - 1U : 1U; }

int parallel_task_start(void) {
    if (parallel_core_id() || !task_poll || active || parallel_load(&tasks_active)) return 0;
    parallel_store(&tasks_active, 1);
    parallel_wake();
    return 1;
}

void parallel_task_stop(void) {
    if (!parallel_core_id()) parallel_store(&tasks_active, 0);
}

typedef struct {
    uint32_t entered[PARALLEL_CORES];
    uint32_t correct[PARALLEL_CORES];
    uint32_t sums[PARALLEL_CORES];
} SelfTest;

static uint32_t checksum(unsigned int part, unsigned int parts) {
    uint32_t sum = 0;
    for (uint32_t i = part; i < 65536; i += parts) sum += i ^ 0x5a5au;
    return sum;
}

static void self_test_job(unsigned int part, unsigned int parts, void* context) {
    SelfTest* test = context;
    if (parts != PARALLEL_CORES || parallel_core_id() != part) return;
    parallel_store(&test->entered[part], 1);
    parallel_wake();
    /* Every physical core must be inside this callback before any can pass. */
    for (unsigned int core = 0; core < parts; core++) {
        if (!wait_for(&test->entered[core], 1)) return;
    }
    test->sums[part] = checksum(part, parts);
    test->correct[part] = 1;
}

uint32_t parallel_self_test(void) {
    if (parallel_core_id() != 0 || !enabled || active) return 0;
    for (unsigned int round = 0; round < 8; round++) {
        SelfTest test = { {0}, {0}, {0} };
        if (!parallel_run(self_test_job, &test)) return 0;
        uint64_t total = 0;
        for (unsigned int core = 0; core < PARALLEL_CORES; core++) {
            if (!test.correct[core] || test.sums[core] != checksum(core, PARALLEL_CORES))
                return 0;
            total += test.sums[core];
        }
        /* XOR permutes all 16-bit inputs, so their sum is independently known. */
        if (total != 2147450880ULL) return 0;
    }
    return 1;
}
