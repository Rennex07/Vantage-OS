#include <vantage/kernel/task.h>
#include <vantage/kernel/parallel_port.h>

typedef struct {
    unsigned int count[2];
    unsigned int failed;
} Progress;

typedef struct { Progress* progress; unsigned int slot; } Participant;

static void probe(void* argument) {
    Participant* participant = argument;
    Progress* progress = participant->progress;
    unsigned int slot = participant->slot;
    volatile uint64_t local = 0x123456789abcdef0ULL + slot;
    for (unsigned int i = 0; i < 8; i++) {
        if (!task_current() || progress->count[slot ^ 1U] < i ||
            progress->count[slot ^ 1U] > i + 1U ||
            local != 0x123456789abcdef0ULL + slot) progress->failed = 1;
        progress->count[slot]++;
        if (task_yield() != TASK_OK) progress->failed = 1;
    }
    task_exit((int)slot + 10);
}

int task_self_test(void) {
    if (task_core() || task_current()) return 0;
    static Progress progress;
    static Participant participants[2] = {{&progress, 0}, {&progress, 1}};
    progress = (Progress){{0, 0}, 0};
    TaskId first = task_create(probe, &participants[0]);
    TaskId second = task_create(probe, &participants[1]);
    int a = 0, b = 0;
    if (!first || !second) {
        return 0;
    }
    return task_run() == TASK_OK && task_reap(first, &a) == TASK_OK &&
           task_reap(second, &b) == TASK_OK && a == 10 && b == 11 &&
           progress.count[0] == 8 && progress.count[1] == 8 && !progress.failed;
}

static uint32_t core_entered[TASK_CORES];
static uint32_t core_passed[TASK_CORES];

static void core_probe(void* argument) {
    unsigned int core = (unsigned int)(uintptr_t)argument;
    volatile uint64_t preserved = 0x123456789abcdef0ULL ^ core;
    if (task_core() != core) task_exit(-1);
    parallel_store(&core_entered[core], 1);
    parallel_wake();
    uint32_t start = parallel_time_us(), remaining = 0x4000000U;
    for (;;) {
        unsigned int count = 0;
        for (unsigned int i = 0; i < TASK_CORES; i++) count += parallel_load(&core_entered[i]);
        if (count == TASK_CORES) break;
        if (parallel_time_us() - start >= 1000000U || !--remaining) task_exit(-2);
    }
    for (unsigned int i = 0; i < 64; i++) {
        if (!task_current() || task_core() != core ||
            preserved != (0x123456789abcdef0ULL ^ core) || task_yield() != TASK_OK) task_exit(-3);
    }
    parallel_store(&core_passed[core], 1);
    task_exit(100 + (int)core);
}

static void preemption_probe(void* argument) {
    unsigned int core = (unsigned int)(uintptr_t)argument;
    TaskInfo info;
    if (task_core() != core || task_preempt_begin() != TASK_OK) task_exit(-1);
    /* Interrupt-enabled: only the clock is read until the region is closed. */
    uint32_t start = parallel_time_us(), remaining = 0x4000000U;
    while (parallel_time_us() - start < 3U * TASK_SLICE_US && --remaining) {}
    task_preempt_end();
    if (task_info(task_current(), &info) != TASK_OK || !info.preemptions) task_exit(-2);
    task_exit(200 + (int)task_core());
}

int task_preemption_self_test(void) {
    if (task_core() || task_current()) return 0;
    uint32_t mask = parallel_task_mask();
    TaskId ids[TASK_CORES] = {0};
    for (unsigned int core = 0; core < TASK_CORES; core++) {
        if (!(mask & (1U << core))) continue;
        ids[core] = task_create_affinity(preemption_probe, (void*)(uintptr_t)core, 1U << core);
        if (!ids[core]) return 0;
    }
    if (task_run() != TASK_OK) return 0;
    int passed = 1;
    for (unsigned int core = 0; core < TASK_CORES; core++) {
        int status = 0;
        if (ids[core] && (task_reap(ids[core], &status) != TASK_OK || status != 200 + (int)core)) passed = 0;
    }
    return passed;
}

int task_multicore_self_test(void) {
    if (task_core() || task_current() || parallel_task_mask() != TASK_ALL_CORES) return 0;
    TaskId ids[TASK_CORES];
    for (unsigned int core = 0; core < TASK_CORES; core++) {
        parallel_store(&core_entered[core], 0);
        parallel_store(&core_passed[core], 0);
        ids[core] = task_create_affinity(core_probe, (void*)(uintptr_t)core, 1U << core);
        if (!ids[core]) return 0;
    }
    if (task_run() != TASK_OK) return 0;
    for (unsigned int core = 0; core < TASK_CORES; core++) {
        int status = 0;
        if (task_reap(ids[core], &status) != TASK_OK || status != 100 + (int)core ||
            !parallel_load(&core_passed[core])) return 0;
    }
    return 1;
}
