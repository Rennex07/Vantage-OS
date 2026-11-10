/* Process lifecycle, address-space bookkeeping and ASID reuse on the host
 * port. EL0 code is modeled by host functions; ARM permission enforcement is
 * covered by tests/arm/processes.py, not here. */
#define VANTAGE_PROCESS_MODEL
#include "host_port.h"
#include <vantage/kernel/user_access.h>
#include "kernel/sched/internal.h"
#include <string.h>

#define PAGE 4096U
#define TEXT_PAGES 2U
#define DATA_BYTES 24U
#define BSS_BYTES 6000U
#define DATA_ADDRESS (USER_BASE + (TEXT_PAGES + 1U) * PAGE)
#define IMAGE_BYTES ((TEXT_PAGES + 1U) * PAGE + DATA_BYTES)

static TASK_ALIGN unsigned char image[IMAGE_BYTES];

static const void* make_image(unsigned int program, uint64_t marker) {
    ProcessImage header = {PROCESS_IMAGE_MAGIC, HOST_ENTRY(program), TEXT_PAGES * PAGE, PAGE,
                           DATA_BYTES, BSS_BYTES};
    memset(image, 0, sizeof(image));
    memcpy(image, &header, sizeof(header));
    memcpy(image + TEXT_PAGES * PAGE, "constant", 8);
    memcpy(image + (TEXT_PAGES + 1U) * PAGE, &marker, sizeof(marker));
    return image;
}

static ProcessId spawn(unsigned int program, uint64_t marker, uint32_t affinity) {
    return process_create(make_image(program, marker), IMAGE_BYTES, affinity);
}

static ProcessInfo info_of(ProcessId id) {
    ProcessInfo info;
    CHECK(process_info(id, &info) == TASK_OK);
    return info;
}

/* --- Modeled user programs ------------------------------------------- */
static uint32_t ran[8];
static uint64_t seen_marker[TASK_CORES];

static void check_installed(void) {
    ProcessInfo info = info_of(process_current());
    CHECK(info.asid && (host_root[core_id] >> 48) == info.asid);
}

static void exit_seven(uint64_t argument) {
    CHECK(!argument && process_current() && task_current());
    check_installed();
    parallel_store(&ran[0], parallel_load(&ran[0]) + 1);
    task_exit(7);
}

/* Reads its own data, yields, and requires the same data afterwards. */
static void marker_program(uint64_t argument) {
    (void)argument;
    uint64_t first = 0, again = 0, zero = 1;
    char constant[8];
    CHECK(user_copy_in(&first, DATA_ADDRESS, sizeof(first)));
    CHECK(user_copy_in(&zero, DATA_ADDRESS + DATA_BYTES + 4096, sizeof(zero)) && !zero);
    CHECK(user_copy_in(constant, USER_BASE + TEXT_PAGES * PAGE, 8) && !memcmp(constant, "constant", 8));
    for (unsigned int i = 0; i < 16; i++) {
        uint64_t next = first + i;
        CHECK(user_copy_out(DATA_ADDRESS + 8, &next, sizeof(next)));
        CHECK(task_yield() == TASK_OK);
        check_installed();
        CHECK(user_copy_in(&again, DATA_ADDRESS + 8, sizeof(again)) && again == next);
    }
    seen_marker[first & 3U] = first;
    task_exit((int)first);
}

static uint64_t bad_address;
static uint32_t bad_write, survived;
static void bad_pointer(uint64_t argument) {
    (void)argument;
    unsigned char byte = 0;
    /* A range the process may not access never returns to the process. */
    if (bad_write) (void)user_copy_out((uintptr_t)bad_address, &byte, 1);
    else (void)user_copy_in(&byte, (uintptr_t)bad_address, 1);
    parallel_store(&survived, 1);
    task_exit(0);
}

static uint32_t release_spinner, spinner_resumed, second_ran;
static void spinner(uint64_t argument) {
    (void)argument;
    while (!parallel_load(&release_spinner)) {
        CHECK(task_yield() == TASK_OK);
        parallel_store(&spinner_resumed, parallel_load(&spinner_resumed) + 1);
    }
    task_exit(1);
}

static void second_thread(uint64_t argument) {
    uint64_t shared = 0;
    CHECK(argument == 0x5ec0 && task_core() == 2);
    check_installed();
    /* Threads share the one address space of their process. */
    CHECK(user_copy_in(&shared, DATA_ADDRESS, sizeof(shared)) && shared == 0x77);
    parallel_store(&second_ran, 1);
    task_exit(5);
}

/* --- Tests ------------------------------------------------------------- */
static void test_lifecycle(void) {
    unsigned int before = pages_live, released_before = released;
    ProcessId id = spawn(0, 1, TASK_CORE0);
    CHECK(id && pages_live > before && code_syncs == 1);
    ProcessInfo info = info_of(id);
    CHECK(info.state == PROCESS_LIVE && info.threads == 1 && info.live == 1 && info.asid == 1);
    /* Tables (root, user level 2, two level 3) plus text, rodata, data/bss and stack. */
    CHECK(info.pages == 4U + TEXT_PAGES + 1U + 2U + PROCESS_STACK_BYTES / PAGE && info.pages == pages_live - before);
    int status = 0;
    CHECK(process_reap(id, &status) == TASK_PENDING && !flushes);
    CHECK(task_run() == TASK_OK && parallel_load(&ran[0]) == 1);
    for (unsigned int core = 0; core < TASK_CORES; core++) CHECK(!host_root[core]);
    info = info_of(id);
    CHECK(info.state == PROCESS_EXITED && !info.live && info.status == 7);
    CHECK(released == released_before + 1 && last_released == id);
    CHECK(process_reap(id, &status) == TASK_OK && status == 7);
    CHECK(pages_live == before && flushed[1] == 1 && last_released == id);
    CHECK(process_reap(id, &status) == TASK_INVALID && process_info(id, &info) == TASK_INVALID);
    CHECK(process_kill(id, 1) == TASK_INVALID && !process_current());
    puts("PASS: process create, run, exit status, reap and complete memory release");
}

static void rollover(void* argument) {
    (void)argument;
    /* One process stays alive across the wrap: its ASID must be skipped. */
    ProcessId resident = spawn(3, 0, TASK_CORE0);
    uint32_t resident_asid = info_of(resident).asid, expected = resident_asid;
    CHECK(resident && resident_asid == 2);
    uint32_t base_flushes = flushes;
    for (unsigned int round = 0; round < 600; round++) {
        ProcessId id = spawn(0, round, TASK_CORE0);
        CHECK(id);
        uint32_t asid = info_of(id).asid;
        expected = expected % 255U + 1U;
        if (expected == resident_asid) expected = expected % 255U + 1U;
        CHECK(asid == expected && asid != resident_asid && asid >= 1 && asid <= 255);
        int status = 0;
        CHECK(process_wait(id, &status) == TASK_OK && status == 7);
        CHECK(flushes == base_flushes + round + 1);
    }
    CHECK(flushed[resident_asid] == 0 && flushed[3] >= 2 && flushed[255] >= 2);
    parallel_store(&release_spinner, 1);
    CHECK(process_wait(resident, NULL) == TASK_OK && flushed[resident_asid] == 1);
    parallel_store(&release_spinner, 0);
}

static void test_asid_rollover(void) {
    TaskId coordinator = task_create(rollover, NULL);
    CHECK(coordinator && task_run() == TASK_OK && task_reap(coordinator, NULL) == TASK_OK);
    puts("PASS: 600 address spaces reuse 255 ASIDs across rollover, each flushed before reuse");
}

static void test_capacity_and_failed_creation(void) {
    unsigned int before = pages_live;
    ProcessId ids[PROCESS_LIMIT + 1U];
    for (unsigned int i = 0; i < PROCESS_LIMIT; i++) CHECK((ids[i] = spawn(0, i, TASK_CORE0)) != 0);
    CHECK(!spawn(0, 99, TASK_CORE0));
    CHECK(task_run() == TASK_OK);
    CHECK(!spawn(0, 99, TASK_CORE0)); /* Exited processes hold their slot until reaped. */
    for (unsigned int i = 0; i < PROCESS_LIMIT; i++) CHECK(process_reap(ids[i], NULL) == TASK_OK);
    CHECK(pages_live == before);

    /* Every possible allocation failure rolls back completely. */
    uint32_t next_asid = 0;
    for (int64_t budget = 0;; budget++) {
        page_budget = budget;
        ProcessId id = spawn(0, 5, TASK_CORE0);
        page_budget = -1;
        if (id) {
            CHECK(budget == 13 && (next_asid = info_of(id).asid) != 0);
            CHECK(task_run() == TASK_OK && process_reap(id, NULL) == TASK_OK);
            break;
        }
        CHECK(pages_live == before && budget < 13);
    }
    ProcessId id = spawn(0, 5, TASK_CORE0);
    /* Failed attempts took and returned an ASID each, like a short-lived process. */
    CHECK(id && info_of(id).asid != next_asid);
    CHECK(task_run() == TASK_OK && process_reap(id, NULL) == TASK_OK && pages_live == before);

    ProcessImage header;
    make_image(0, 0);
    memcpy(&header, image, sizeof(header));
    ProcessImage broken[] = {header, header, header, header, header, header, header};
    broken[0].magic++;
    broken[1].entry = USER_BASE + header.text_bytes;
    broken[2].entry += 2;
    broken[3].text_bytes += 1;
    broken[4].data_bytes += 1;
    broken[5].bss_bytes = PROCESS_IMAGE_LIMIT + 1U;
    broken[6].entry = USER_BASE;
    for (unsigned int i = 0; i < sizeof(broken) / sizeof(broken[0]); i++) {
        memcpy(image, &broken[i], sizeof(header));
        CHECK(!process_create(image, IMAGE_BYTES, TASK_CORE0));
    }
    make_image(0, 0);
    CHECK(!process_create(image, IMAGE_BYTES - 1, TASK_CORE0) && !process_create(image, 40, TASK_CORE0));
    CHECK(!process_create(NULL, IMAGE_BYTES, TASK_CORE0) && !process_create(image + 4, IMAGE_BYTES, TASK_CORE0));
    CHECK(!process_create(image, IMAGE_BYTES, 0) && !process_create(image, IMAGE_BYTES, 16));
    CHECK(pages_live == before);
    puts("PASS: process capacity, allocation-failure rollback and rejected images");
}

static const uint64_t* leaf_of(const Process* process, uint64_t address) {
    const uint64_t* root = process->space.root;
    const uint64_t* middle = (const uint64_t*)(uintptr_t)(root[4] & 0x0000fffffffff000ULL);
    CHECK((root[4] & 3U) == 3U && middle == process->space.user);
    uint64_t entry = middle[(address >> 21) & 511U];
    if (!(entry & 1U)) return NULL;
    return &((const uint64_t*)(uintptr_t)(entry & 0x0000fffffffff000ULL))[(address >> 12) & 511U];
}

static void test_isolated_spaces(void) {
    ProcessId a = spawn(1, 0x1001, TASK_CORE0), b = spawn(1, 0x2002, TASK_ALL_CORES);
    CHECK(a && b && info_of(a).asid != info_of(b).asid);
    sched_lock();
    const Process* first = process_lookup(a);
    const Process* second = process_lookup(b);
    sched_unlock();
    CHECK(first->root != second->root && (first->root >> 48) == first->asid);
    for (unsigned int i = 0; i < 4; i++)
        CHECK(first->space.root[i] == kernel_level1[i] && second->space.root[i] == kernel_level1[i]);
    for (unsigned int i = 5; i < 512; i++) CHECK(!first->space.root[i]);
    /* Same virtual address, different pages and contents. */
    const uint64_t* data_a = leaf_of(first, DATA_ADDRESS);
    const uint64_t* data_b = leaf_of(second, DATA_ADDRESS);
    CHECK(data_a && data_b && (*data_a & 0x0000fffffffff000ULL) != (*data_b & 0x0000fffffffff000ULL));
    uint64_t value = 0;
    CHECK(space_copy_in(&first->space, &value, DATA_ADDRESS, 8) && value == 0x1001);
    CHECK(space_copy_in(&second->space, &value, DATA_ADDRESS, 8) && value == 0x2002);
    /* EL0 access, not global, never privileged-executable; W^X per region. */
    const uint64_t user = (1ULL << 6) | (1ULL << 11) | (1ULL << 53) | 3U;
    const uint64_t read_only = 1ULL << 7, no_execute = 1ULL << 54;
    const uint64_t* text = leaf_of(first, USER_BASE);
    const uint64_t* rodata = leaf_of(first, USER_BASE + TEXT_PAGES * PAGE);
    const uint64_t* stack = leaf_of(first, USER_BASE + USER_BYTES - PAGE);
    CHECK(text && rodata && stack);
    CHECK((*text & (user | read_only | no_execute)) == (user | read_only));
    CHECK((*rodata & (user | read_only | no_execute)) == (user | read_only | no_execute));
    CHECK((*data_a & (user | read_only | no_execute)) == (user | no_execute));
    CHECK((*stack & (user | read_only | no_execute)) == (user | no_execute));
    /* Unmapped guard below the stack and past the bss. */
    const uint64_t* guard = leaf_of(first, USER_BASE + USER_BYTES - PROCESS_STACK_BYTES - PAGE);
    const uint64_t* past = leaf_of(first, DATA_ADDRESS + 2U * PAGE);
    CHECK((!guard || !*guard) && (!past || !*past));
    CHECK(!space_accessible(&first->space, DATA_ADDRESS + 2U * PAGE - 4, 8, SPACE_READ));
    CHECK(space_accessible(&first->space, DATA_ADDRESS + 2U * PAGE - 8, 8, SPACE_WRITE));
    CHECK(!space_accessible(&first->space, USER_BASE, 8, SPACE_WRITE));
    CHECK(space_accessible(&first->space, USER_BASE, 8, SPACE_EXECUTE));
    CHECK(!space_accessible(&first->space, DATA_ADDRESS, 8, SPACE_EXECUTE));
    CHECK(!space_accessible(&first->space, DATA_ADDRESS, 0, SPACE_READ));
    CHECK(!space_accessible(&first->space, USER_BASE - 8, 16, SPACE_READ));
    CHECK(!space_accessible(&first->space, (uint64_t)(uintptr_t)&value, 8, SPACE_READ));
    CHECK(!space_accessible(&first->space, ~(uint64_t)0 - 3, 8, SPACE_READ));
    CHECK(task_run() == TASK_OK && seen_marker[1] == 0x1001 && seen_marker[2] == 0x2002);
    int status = 0;
    CHECK(process_reap(a, &status) == TASK_OK && status == 0x1001);
    CHECK(process_reap(b, &status) == TASK_OK && status == 0x2002);

    AddressSpace space;
    CHECK(space_create(&space, kernel_level1));
    CHECK(!space_map(&space, USER_BASE, 1, SPACE_READ | SPACE_WRITE | SPACE_EXECUTE));
    CHECK(!space_map(&space, USER_BASE, 1, SPACE_WRITE) && !space_map(&space, USER_BASE + 1, 1, SPACE_READ));
    CHECK(!space_map(&space, USER_BASE - PAGE, 1, SPACE_READ) && !space_map(&space, USER_BASE + USER_BYTES, 1, SPACE_READ));
    CHECK(!space_map(&space, USER_BASE + USER_BYTES - PAGE, 2, SPACE_READ) && !space_map(&space, USER_BASE, 0, SPACE_READ));
    CHECK(space_map(&space, USER_BASE, 1, SPACE_READ) && !space_map(&space, USER_BASE, 1, SPACE_READ));
    space_destroy(&space);
    CHECK(!space.root && !space.pages);
    puts("PASS: per-process tables, same-address isolation, W^X and range checks");
}

static void expect_bad_pointer(uint64_t address, uint32_t write) {
    unsigned int before = pages_live;
    bad_address = address;
    bad_write = write;
    parallel_store(&survived, 0);
    ProcessId id = spawn(2, 0, TASK_CORE0);
    int status = 0;
    CHECK(id && task_run() == TASK_OK && !parallel_load(&survived));
    CHECK(process_reap(id, &status) == TASK_OK && status == PROCESS_FAULT_ADDRESS && pages_live == before);
}

static void test_bad_syscall_buffers_terminate_only_the_caller(void) {
    static uint64_t kernel_word = 0x1234;
    expect_bad_pointer((uint64_t)(uintptr_t)&kernel_word, 0);
    expect_bad_pointer((uint64_t)(uintptr_t)&kernel_word, 1);
    expect_bad_pointer(0, 0);
    expect_bad_pointer(DATA_ADDRESS + 2U * PAGE, 0);       /* Past the bss. */
    expect_bad_pointer(USER_BASE + 64, 1);                 /* Text is read-only. */
    expect_bad_pointer(USER_BASE + TEXT_PAGES * PAGE, 1);  /* So are constants. */
    expect_bad_pointer(USER_BASE + USER_BYTES, 0);
    expect_bad_pointer(~(uint64_t)0, 0);
    CHECK(kernel_word == 0x1234);
    /* Kernel callers have no address space: rejected without any effect. */
    uint64_t value = 0;
    CHECK(!user_copy_in(&value, DATA_ADDRESS, 8) && !user_copy_out(DATA_ADDRESS, &value, 8));
    CHECK(!user_access_ok(DATA_ADDRESS, 8, 0));
    process_fault(PROCESS_FAULT_DATA);
    puts("PASS: forbidden syscall buffers terminate the calling process only");
}

static uint32_t waited;
static void waiter_task(void* argument) {
    ProcessId id = *(ProcessId*)argument;
    int status = 0;
    CHECK(task_core() == 0 && process_wait(id, &status) == TASK_OK && status == 5);
    parallel_store(&waited, 1);
}

static void remote_creator(void* argument) {
    (void)argument;
    /* Only core 0 owns the page allocator. */
    CHECK(task_core() == 1 && !spawn(0, 0, 2) && !process_thread(*(ProcessId*)argument, HOST_ENTRY(0), 0, 2));
    CHECK(process_reap(*(ProcessId*)argument, NULL) == TASK_INVALID);
}

static void test_threads_exit_and_kill(void) {
    unsigned int before = pages_live;
    ProcessId id = spawn(3, 0x77, TASK_CORE0);
    CHECK(id && !process_thread(id, HOST_ENTRY(4) + 2, 0, 4) && !process_thread(id, DATA_ADDRESS, 0, 4));
    TaskId thread = process_thread(id, HOST_ENTRY(4), 0x5ec0, 4);
    CHECK(thread && info_of(id).threads == 2 && info_of(id).live == 2);
    int status = 0;
    CHECK(task_reap(thread, &status) == TASK_INVALID);
    TaskId waiter = task_create(waiter_task, &id);
    TaskId remote = task_create_affinity(remote_creator, &id, 2);
    CHECK(waiter && remote && task_run() == TASK_OK);
    /* The second thread's exit ended the process: the first never ran again. */
    CHECK(parallel_load(&second_ran) && parallel_load(&waited));
    CHECK(process_info(id, &(ProcessInfo){0}) == TASK_INVALID && pages_live == before);
    CHECK(task_reap(waiter, NULL) == TASK_OK && task_reap(remote, NULL) == TASK_OK);

    /* A kernel-requested kill stops a process that would never exit. */
    id = spawn(3, 0, TASK_ALL_CORES);
    ProcessId peer = spawn(1, 0x3003, TASK_ALL_CORES);
    CHECK(id && peer && process_kill(id, -9) == TASK_OK && process_kill(id, -10) == TASK_OK);
    CHECK(task_run() == TASK_OK);
    CHECK(process_reap(id, &status) == TASK_OK && status == -9);
    CHECK(process_reap(peer, &status) == TASK_OK && status == 0x3003 && pages_live == before);
    for (unsigned int i = 0; i < PROCESS_THREADS; i++) {
        id = spawn(3, 0, TASK_CORE0);
        for (unsigned int extra = 0; extra < i; extra++) CHECK(process_thread(id, HOST_ENTRY(3), 0, TASK_ALL_CORES));
        if (i == PROCESS_THREADS - 1) CHECK(!process_thread(id, HOST_ENTRY(3), 0, TASK_ALL_CORES));
        CHECK(process_kill(id, 3) == TASK_OK && task_run() == TASK_OK && process_reap(id, &status) == TASK_OK);
        CHECK(status == 3 && pages_live == before);
    }
    puts("PASS: threads share one space; exit and kill end every thread; core 0 owns teardown");
}

int main(void) {
    host_programs[0] = exit_seven;
    host_programs[1] = marker_program;
    host_programs[2] = bad_pointer;
    host_programs[3] = spinner;
    host_programs[4] = second_thread;
    host_start_workers();
    CHECK(parallel_init() && task_init() == TASK_OK);
    wait_flag(&allow_workers);
    test_lifecycle();
    test_asid_rollover();
    test_capacity_and_failed_creation();
    test_isolated_spaces();
    test_bad_syscall_buffers_terminate_only_the_caller();
    test_threads_exit_and_kill();
    host_stop_workers();
    CHECK(!pages_live);
    puts("PASS: all process tests (host model; ARM isolation is tested on the CPU model)");
    return 0;
}
