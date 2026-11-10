/* Execute the production scheduler on four OS threads. Host fibers/ucontext
 * replace only the ARM register switch; they do not prove Pi cache coherence. */
#include "host_port.h"

static uint32_t migrated, migration_started;
static void migrant(void* argument) {
    (void)argument;
    CHECK(task_core() == 0 && !task_services_allowed());
    TaskId self = task_current();
    volatile uint64_t local = 0xaabbccddeeff0011ULL;
    parallel_store(&migration_started, 1);
    parallel_store(&allow_workers, 1);
    CHECK(task_yield() == TASK_OK);
    CHECK(task_core() > 0 && task_core() < TASK_CORES && task_current() == self);
    CHECK(local == 0xaabbccddeeff0011ULL && !task_services_allowed());
    parallel_store(&migrated, 1);
}
static void blocker(void* argument) {
    (void)argument;
    while (!parallel_load(&migration_started)) CHECK(task_yield() == TASK_OK);
    wait_flag(&migrated);
}

static void test_migration(void) {
    parallel_store(&allow_workers, 0);
    for (unsigned int core = 1; core < TASK_CORES; core++) wait_flag(&parked[core]);
    TaskId a = task_create_affinity(migrant, NULL, TASK_ALL_CORES);
    TaskId b = task_create(blocker, NULL);
    CHECK(a && b && task_run() == TASK_OK && parallel_load(&migrated));
    TaskInfo info;
    CHECK(task_info(a, &info) == TASK_OK && info.dispatches == 2 && info.core == TASK_CORES);
    CHECK(task_reap(a, NULL) == TASK_OK && task_reap(b, NULL) == TASK_OK);
    puts("PASS: live task migrated between cores after yielding");
}

static TaskId target_id, waiter_id;
static uint32_t thief_done;
static int join_status;
static void join_waiter(void* argument) {
    (void)argument;
    CHECK(task_join(target_id, &join_status) == TASK_OK && join_status == 73);
}
static void join_thief(void* argument) {
    (void)argument;
    TaskInfo info;
    do { CHECK(task_info(waiter_id, &info) == TASK_OK); CHECK(task_yield() == TASK_OK); }
    while (info.state != TASK_BLOCKED);
    CHECK(task_reap(target_id, NULL) == TASK_BUSY && task_join(target_id, NULL) == TASK_BUSY);
    parallel_store(&thief_done, 1);
}
static void join_target(void* argument) {
    (void)argument;
    while (!parallel_load(&thief_done)) CHECK(task_yield() == TASK_OK);
    task_exit(73);
}
static void test_cross_core_join(void) {
    waiter_id = task_create(join_waiter, NULL);
    target_id = task_create_affinity(join_target, NULL, 2);
    TaskId thief = task_create_affinity(join_thief, NULL, 4);
    CHECK(waiter_id && target_id && thief && task_run() == TASK_OK);
    CHECK(task_reap(target_id, NULL) == TASK_INVALID);
    CHECK(task_reap(waiter_id, NULL) == TASK_OK && task_reap(thief, NULL) == TASK_OK);
    puts("PASS: cross-core join/wakeup and exclusive reaping");
}

static uint32_t roots_ready[TASK_CORES], full_checked[TASK_CORES], children_done[TASK_CORES];
static void child(void* argument) {
    unsigned int parent = (unsigned int)(uintptr_t)argument;
    volatile uint64_t local = 0x1234567890abcdefULL;
    for (unsigned int i = 0; i < 32; i++) {
        CHECK(task_current() && task_core() < TASK_CORES);
        CHECK(task_yield() == TASK_OK && local == 0x1234567890abcdefULL);
    }
    /* Child completion is counted by its parent after joining. */
    task_exit((int)parent + 10);
}
static void creator(void* argument) {
    unsigned int parent = (unsigned int)(uintptr_t)argument;
    TaskId children[7];
    for (unsigned int i = 0; i < 7; i++) {
        children[i] = task_create_affinity(child, argument, TASK_ALL_CORES);
        CHECK(children[i]);
    }
    parallel_store(&roots_ready[parent], 1);
    for (unsigned int core = 0; core < TASK_CORES; core++) wait_flag(&roots_ready[core]);
    CHECK(!task_create_affinity(child, argument, TASK_ALL_CORES));
    parallel_store(&full_checked[parent], 1);
    for (unsigned int core = 0; core < TASK_CORES; core++) wait_flag(&full_checked[core]);
    for (unsigned int i = 0; i < 7; i++) {
        int status = -1;
        CHECK(task_join(children[i], &status) == TASK_OK && status == (int)parent + 10);
    }
    parallel_store(&children_done[parent], 1);
}
static void test_concurrent_creation(void) {
    for (unsigned int round = 0; round < 32; round++) {
        TaskId roots[TASK_CORES];
        for (unsigned int core = 0; core < TASK_CORES; core++) {
            parallel_store(&roots_ready[core], 0); parallel_store(&full_checked[core], 0);
            parallel_store(&children_done[core], 0);
            roots[core] = task_create_affinity(creator, (void*)(uintptr_t)core, 1U << core);
            CHECK(roots[core]);
        }
        CHECK(task_run() == TASK_OK);
        for (unsigned int core = 0; core < TASK_CORES; core++) {
            CHECK(parallel_load(&children_done[core]) && task_reap(roots[core], NULL) == TASK_OK);
        }
    }
    puts("PASS: concurrent creation, 32-slot capacity, 28 movable children, 32 stress rounds");
}

static unsigned int raster_parts;
static uint32_t compute_entered, compute_release;
static void raster(unsigned int part, unsigned int parts, void* argument) {
    (void)argument;
    CHECK(part == 0 || !task_current());
    if (!part) raster_parts = parts;
}
static void busy_compute(void* argument) {
    (void)argument;
    parallel_store(&compute_entered, 1);
    wait_flag(&compute_release);
}
static void render_task(void* argument) {
    (void)argument;
    CHECK(task_services_allowed());
    wait_flag(&compute_entered);
    CHECK(parallel_run(raster, NULL) && raster_parts == 1);
    parallel_store(&compute_release, 1);
}
static void idle_render_task(void* argument) {
    (void)argument;
    CHECK(parallel_run(raster, NULL) && raster_parts == TASK_CORES);
}
static void compute_stress(void* argument) {
    (void)argument;
    for (unsigned int i = 0; i < 512; i++) CHECK(task_yield() == TASK_OK);
}
static void render_stress(void* argument) {
    (void)argument;
    for (unsigned int i = 0; i < 256; i++) {
        CHECK(parallel_run(raster, NULL) && (raster_parts == 1 || raster_parts == TASK_CORES));
        CHECK(task_yield() == TASK_OK);
    }
}

int main(void) {
    host_start_workers();
    CHECK(parallel_init() && parallel_self_test());
    CHECK(task_init() == TASK_OK && task_init() == TASK_INVALID);
    CHECK(!task_create_affinity(child, NULL, 0) && !task_create_affinity(child, NULL, 16));
    CHECK(task_self_test() && task_multicore_self_test());
    puts("PASS: simultaneous four-core entry, affinity, stacks and repeated yields");
    test_migration(); test_cross_core_join(); test_concurrent_creation();
    TaskId render = task_create(render_task, NULL);
    TaskId compute = task_create_affinity(busy_compute, NULL, 2);
    CHECK(render && compute && task_run() == TASK_OK);
    CHECK(task_reap(render, NULL) == TASK_OK && task_reap(compute, NULL) == TASK_OK);
    render = task_create(idle_render_task, NULL);
    CHECK(render && task_run() == TASK_OK && task_reap(render, NULL) == TASK_OK);
    CHECK(parallel_run(raster, NULL) && raster_parts == TASK_CORES);
    TaskId mixed[TASK_CORES];
    mixed[0] = task_create(render_stress, NULL);
    for (unsigned int core = 1; core < TASK_CORES; core++)
        mixed[core] = task_create_affinity(compute_stress, NULL, 1U << core);
    for (unsigned int core = 0; core < TASK_CORES; core++) CHECK(mixed[core]);
    CHECK(task_run() == TASK_OK);
    for (unsigned int core = 0; core < TASK_CORES; core++) CHECK(task_reap(mixed[core], NULL) == TASK_OK);
    puts("PASS: occupied-worker serial fallback, idle-worker reservation and four-way batches");
    host_stop_workers();
    CHECK(released > 0);
    puts("PASS: all scheduler thread/fiber tests (physical Pi validation pending)");
    return 0;
}
