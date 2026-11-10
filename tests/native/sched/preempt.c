/* Preemption by a simulated per-core tick on the host port. The scheduler's
 * tick, requeue, migration and termination logic is the production code; the
 * interrupt itself is modeled at explicit points (see host_port.h). Real
 * interrupt frames are tested on the ARM model in tests/arm/preemption.py. */
#define VANTAGE_PROCESS_MODEL
#include "host_port.h"
#include <vantage/kernel/user_access.h>
#include <string.h>

#define PAGE 4096U
#define IMAGE_BYTES (PAGE + 8U)
#define DATA_ADDRESS (USER_BASE + PAGE)

static unsigned char image[IMAGE_BYTES + 8U];

static ProcessId spawn(unsigned int program, uint64_t marker, uint32_t affinity) {
    ProcessImage header = {PROCESS_IMAGE_MAGIC, HOST_ENTRY(program), PAGE, 0, 8, 0};
    memset(image, 0, sizeof(image));
    memcpy(image, &header, sizeof(header));
    memcpy(image + PAGE, &marker, sizeof(marker));
    return process_create(image, IMAGE_BYTES, affinity);
}

static uint32_t total_ticks(void) {
    uint32_t total = 0;
    for (unsigned int core = 0; core < TASK_CORES; core++) total += ticks_taken[core];
    return total;
}

/* --- Modeled user programs: none of them ever yields ------------------- */
static uint32_t progress[4], finish_line, cores_seen, hog_started;

static void racer(uint64_t argument) {
    (void)argument;
    uint64_t lane = 0;
    CHECK(user_copy_in(&lane, DATA_ADDRESS, sizeof(lane)) && lane < 4);
    while (parallel_load(&progress[lane]) < 500) {
        parallel_store(&progress[lane], parallel_load(&progress[lane]) + 1);
        host_user_point();
        /* Still the same process in its own address space after every tick. */
        uint64_t again = ~(uint64_t)0;
        CHECK(user_copy_in(&again, DATA_ADDRESS, sizeof(again)) && again == lane);
        CHECK((host_root[task_core()] >> 48) != 0);
    }
    parallel_store(&finish_line, parallel_load(&finish_line) + 1);
    task_exit(40 + (int)lane);
}

static void wanderer(uint64_t argument) {
    (void)argument;
    volatile uint64_t local = 0x1122334455667788ULL;
    uint32_t start = parallel_time_us();
    for (;;) {
        uint32_t seen = parallel_load(&cores_seen) | (1U << task_core());
        parallel_store(&cores_seen, seen);
        if ((seen & (seen - 1U)) && parallel_load(&finish_line)) break;
        CHECK(parallel_time_us() - start < 20000000U && local == 0x1122334455667788ULL);
        host_user_point();
    }
    task_exit(9);
}

static void hog(uint64_t argument) {
    (void)argument;
    parallel_store(&hog_started, 1);
    for (;;) host_user_point();
}

/* --- Tests ------------------------------------------------------------- */
static void test_non_yielding_processes_share_one_core(void) {
    uint32_t before = total_ticks();
    ProcessId ids[3];
    for (unsigned int lane = 0; lane < 3; lane++) CHECK((ids[lane] = spawn(0, lane, TASK_CORE0)) != 0);
    CHECK(task_run() == TASK_OK && parallel_load(&finish_line) == 3);
    /* 1500 points on one core in slices of HOST_SLICE_POINTS. */
    CHECK(total_ticks() - before >= 1500U / HOST_SLICE_POINTS - 3U && ticks_taken[0] == total_ticks() - before);
    for (unsigned int lane = 0; lane < 3; lane++) {
        int status = 0;
        CHECK(process_reap(ids[lane], &status) == TASK_OK && status == 40 + (int)lane);
    }
    for (unsigned int core = 0; core < TASK_CORES; core++) CHECK(!slice_armed[core] && !host_root[core]);
    puts("PASS: the tick shares one core between processes that never yield");
}

static void test_preempted_process_migrates(void) {
    parallel_store(&cores_seen, 0);
    ProcessId movable = spawn(1, 0, TASK_ALL_CORES);
    ProcessId pinned = spawn(0, 3, TASK_CORE0);
    int status = 0;
    CHECK(movable && pinned && task_run() == TASK_OK);
    uint32_t seen = parallel_load(&cores_seen);
    CHECK(seen & (seen - 1U));
    CHECK(process_reap(movable, &status) == TASK_OK && status == 9);
    CHECK(process_reap(pinned, &status) == TASK_OK && status == 43);
    puts("PASS: a preempted process resumed on another allowed core with its stack intact");
}

static ProcessId hog_id;
static void killer(void* argument) {
    (void)argument;
    int status = 0;
    wait_flag(&hog_started);
    CHECK(process_reap(hog_id, &status) == TASK_PENDING);
    CHECK(process_kill(hog_id, -9) == TASK_OK);
    CHECK(process_wait(hog_id, &status) == TASK_OK && status == -9);
}

static void test_hung_process_is_stopped_by_its_next_tick(void) {
    unsigned int before = pages_live;
    hog_id = spawn(2, 0, 2);
    TaskId coordinator = task_create(killer, NULL);
    CHECK(hog_id && coordinator && task_run() == TASK_OK && task_reap(coordinator, NULL) == TASK_OK);
    /* It loops forever on core 1: only its next tick could have ended it. */
    CHECK(ticks_taken[1] && pages_live == before);
    CHECK(process_info(hog_id, &(ProcessInfo){0}) == TASK_INVALID);
    puts("PASS: a looping process on another core is terminated within one slice");
}

static uint32_t region_open, region_done, peer_ran, masked_points;
static void preemptible_task(void* argument) {
    (void)argument;
    TaskInfo info;
    TaskId self = task_current();
    volatile uint64_t local = 0xfeedfacecafebeefULL;
    CHECK(task_preempt_begin() == TASK_OK);
    /* Interrupt-enabled kernel code is not a scheduler caller. */
    CHECK(task_info(self, &info) == TASK_INVALID && task_yield() == TASK_INVALID && !task_current());
    CHECK(!task_create(preemptible_task, NULL) && !user_access_ok(DATA_ADDRESS, 8, 0));
    parallel_store(&region_open, 1);
    while (!parallel_load(&peer_ran)) host_kernel_point();
    task_preempt_end();
    CHECK(local == 0xfeedfacecafebeefULL && task_current() == self);
    CHECK(task_info(self, &info) == TASK_OK && info.preemptions >= 1 && info.dispatches == info.preemptions + 1);
    parallel_store(&region_done, 1);
}
static void region_peer(void* argument) {
    (void)argument;
    /* Runs on the same core, so only a tick can let it in from here on. */
    while (!parallel_load(&region_open)) CHECK(task_yield() == TASK_OK);
    parallel_store(&peer_ran, 1);
}
static void masked_task(void* argument) {
    (void)argument;
    TaskInfo info;
    /* Masked kernel code: the expired slice stays pending and is never taken. */
    for (unsigned int i = 0; i < 20U * HOST_SLICE_POINTS; i++) {
        host_kernel_point();
        parallel_store(&masked_points, parallel_load(&masked_points) + 1);
    }
    CHECK(task_info(task_current(), &info) == TASK_OK && !info.preemptions && info.dispatches == 1);
}

static void test_kernel_tasks_are_preempted_only_inside_regions(void) {
    uint32_t before = total_ticks();
    TaskId masked = task_create(masked_task, NULL);
    CHECK(masked && task_run() == TASK_OK && total_ticks() == before);
    CHECK(parallel_load(&masked_points) == 20U * HOST_SLICE_POINTS && task_reap(masked, NULL) == TASK_OK);
    CHECK(task_preempt_begin() == TASK_INVALID);   /* Boot context is not a task. */
    task_tick();                                    /* Nor can a tick preempt it. */
    TaskId first = task_create(preemptible_task, NULL), second = task_create(region_peer, NULL);
    CHECK(first && second && task_run() == TASK_OK && parallel_load(&region_done));
    CHECK(total_ticks() > before && task_reap(first, NULL) == TASK_OK && task_reap(second, NULL) == TASK_OK);
    CHECK(task_preemption_self_test());
    for (unsigned int core = 0; core < TASK_CORES; core++) CHECK(ticks_taken[core] && !irq_enabled[core]);
    puts("PASS: kernel tasks are preempted inside explicit regions only, on every core");
}

int main(void) {
    host_programs[0] = racer;
    host_programs[1] = wanderer;
    host_programs[2] = hog;
    host_start_workers();
    CHECK(parallel_init() && task_init() == TASK_OK);
    test_non_yielding_processes_share_one_core();
    test_preempted_process_migrates();
    test_hung_process_is_stopped_by_its_next_tick();
    test_kernel_tasks_are_preempted_only_inside_regions();
    host_stop_workers();
    CHECK(!pages_live && slice_starts > total_ticks());
    puts("PASS: all preemption tests (simulated tick; physical Pi validation pending)");
    return 0;
}
