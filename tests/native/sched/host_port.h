/* Host port shared by the scheduler tests: four OS threads model the cores
 * and fibers/ucontext replace the ARM register switch. Doubles stand in for
 * the page allocator, translation-table maintenance and EL0 execution. */
#pragma once
#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include <vantage/kernel/task_port.h>
#include <vantage/kernel/process.h>
#include <vantage/kernel/memory.h>
#include <vantage/arch/address_space.h>
#include <vantage/arch/mmu.h>
#include <vantage/kernel/parallel_port.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define TLS __declspec(thread)
typedef struct { void* fiber; } HostContext;
static HANDLE threads[TASK_CORES];
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <ucontext.h>
#define TLS _Thread_local
typedef struct { ucontext_t machine; } HostContext;
static pthread_t threads[TASK_CORES];
#endif

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static TLS unsigned int core_id;
static uint32_t stopping, allow_workers = 1, parked[TASK_CORES], released;
static uint64_t last_released;

uint32_t parallel_load(const uint32_t* value) {
#ifdef _WIN32
    return (uint32_t)InterlockedCompareExchange((volatile LONG*)(void*)value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}
void parallel_store(uint32_t* value, uint32_t next) {
#ifdef _WIN32
    InterlockedExchange((volatile LONG*)(void*)value, (LONG)next);
#else
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
#endif
}
unsigned int parallel_core_id(void) { return core_id; }
unsigned int task_port_core(void) { return core_id; }

/* Simulated tick. Each modeled core has a one-shot slice counted in
 * interruptible points: host_user_point() stands for an EL0 instruction
 * boundary and host_kernel_point() for one inside interrupt-enabled EL1 code.
 * When the slice has run out, the point takes the interrupt: it masks, calls
 * the scheduler's tick upcall and resumes, possibly on another core. */
#define HOST_SLICE_POINTS 7U
static uint32_t slice_armed[TASK_CORES], slice_left[TASK_CORES], irq_enabled[TASK_CORES];
static uint32_t ticks_taken[TASK_CORES], slice_starts;
void host_user_point(void);
void host_kernel_point(void);
int task_port_owned(void) { return core_id < TASK_CORES && !irq_enabled[core_id]; }
void task_port_slice_start(uint32_t microseconds) {
    if (microseconds != TASK_SLICE_US || slice_armed[core_id] || irq_enabled[core_id]) {
        fprintf(stderr, "bad slice start on core %u\n", core_id); exit(1);
    }
    slice_armed[core_id] = 1;
    slice_left[core_id] = HOST_SLICE_POINTS;
    slice_starts++;
}
void task_port_slice_stop(void) { slice_armed[core_id] = 0; }
void task_port_irq_enable(void) { irq_enabled[core_id] = 1; }
void task_port_irq_disable(void) { irq_enabled[core_id] = 0; }
static void host_interrupt_point(uint32_t enabled) {
    unsigned int core = core_id;
    if (!slice_armed[core] || (slice_left[core] && --slice_left[core])) return;
    /* Exception entry masks IRQs; the handler stops the level-triggered timer. */
    irq_enabled[core] = 0;
    slice_armed[core] = 0;
    ticks_taken[core]++;
    task_tick();
    task_trap_boundary();
    /* The exception return restores the interrupted context's mask. */
    irq_enabled[task_core()] = enabled;
}
void host_user_point(void) { host_interrupt_point(0); }
void host_kernel_point(void) { if (irq_enabled[core_id]) host_interrupt_point(1); }
uint32_t parallel_time_us(void) {
    host_kernel_point();
#ifdef _WIN32
    LARGE_INTEGER ticks, frequency;
    QueryPerformanceCounter(&ticks); QueryPerformanceFrequency(&frequency);
    return (uint32_t)((ticks.QuadPart / frequency.QuadPart) * 1000000 +
        (ticks.QuadPart % frequency.QuadPart) * 1000000 / frequency.QuadPart);
#else
    struct timespec now;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (uint32_t)((uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U);
#endif
}
void parallel_idle(void) {
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}
void parallel_wake(void) {}
void parallel_completion_barrier(void) {
#ifdef _WIN32
    MemoryBarrier();
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
void parallel_port_halt(const char* message) { fprintf(stderr, "%s", message); exit(1); }
void task_port_halt(const char* message) { parallel_port_halt(message); }
void task_port_lock(uint32_t* lock) {
    for (;;) {
#ifdef _WIN32
        if (!InterlockedExchange((volatile LONG*)(void*)lock, 1)) return;
#else
        if (!__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) return;
#endif
        parallel_idle();
    }
}
void task_port_unlock(uint32_t* lock) { parallel_store(lock, 0); }

#ifdef _WIN32
static VOID WINAPI fiber_entry(void* argument) {
    TaskContext* context = argument;
    void (*entry)(void) = (void (*)(void))(uintptr_t)context->x19_x30[11];
    entry();
    CHECK(0);
}
#endif
void task_context_init(TaskContext* context, void* stack, size_t size, void (*entry)(void)) {
    *context = (TaskContext){0};
    HostContext* host = calloc(1, sizeof(*host));
    CHECK(host);
    context->x19_x30[0] = (uintptr_t)host;
    context->x19_x30[11] = (uintptr_t)entry;
    context->sp = (uintptr_t)stack + size;
#ifdef _WIN32
    host->fiber = CreateFiber(65536, fiber_entry, context);
    CHECK(host->fiber);
#else
    CHECK(!getcontext(&host->machine));
    host->machine.uc_stack.ss_sp = stack;
    host->machine.uc_stack.ss_size = size;
    host->machine.uc_link = NULL;
    makecontext(&host->machine, entry, 0);
#endif
}
void task_context_dispose(TaskContext* context) {
    HostContext* host = (HostContext*)(uintptr_t)context->x19_x30[0];
#ifdef _WIN32
    DeleteFiber(host->fiber);
#endif
    free(host);
    context->x19_x30[0] = 0;
}
void task_context_switch(TaskContext* previous, const TaskContext* next) {
    HostContext* before = (HostContext*)(uintptr_t)previous->x19_x30[0];
    HostContext* after = (HostContext*)(uintptr_t)next->x19_x30[0];
    if (!before) {
        before = calloc(1, sizeof(*before)); CHECK(before);
        previous->x19_x30[0] = (uintptr_t)before;
#ifdef _WIN32
        before->fiber = ConvertThreadToFiber(NULL); CHECK(before->fiber);
#endif
    }
#ifdef _WIN32
    SwitchToFiber(after->fiber);
#else
    CHECK(!swapcontext(&before->machine, &after->machine));
#endif
}
void files_release(uint64_t owner) {
    CHECK(owner && task_services_allowed() && core_id == 0);
    released++;
    last_released = owner;
}

/* The user half each modeled core has installed; 0 is kernel-only. */
static uint64_t host_root[TASK_CORES];
void task_port_address_space(uint64_t root) { host_root[core_id] = root; }

/* EL0 code is a host function selected by the image entry address. It models
 * user execution: it may only enter the kernel as a trap handler would. */
typedef void (*HostUser)(uint64_t argument);
static HostUser host_programs[8];
#define HOST_ENTRY(index) (USER_BASE + sizeof(ProcessImage) + 4U * (index))
void task_port_enter_user(uint64_t entry, uint64_t stack, uint64_t argument) {
    uint64_t index = (entry - HOST_ENTRY(0)) / 4U;
    CHECK(index < 8 && host_programs[index] && stack > USER_BASE && !(stack & 15U));
    host_programs[index](argument);
}

/* Page allocator double: host pages stand in for physical pages. */
static uint32_t pages_live, pages_refused;
static int64_t page_budget = -1;
uint64_t mm_alloc_pages(uint64_t count, uint64_t alignment_pages) {
    CHECK(count == 1 && alignment_pages == 1 && core_id == 0);
    if (page_budget == 0) { pages_refused++; return 0; }
    if (page_budget > 0) page_budget--;
#ifdef _WIN32
    void* page = _aligned_malloc(4096, 4096);
#else
    void* page = aligned_alloc(4096, 4096);
#endif
    CHECK(page);
    for (unsigned int i = 0; i < 4096; i++) ((unsigned char*)page)[i] = 0xa5;
    pages_live++;
    return (uint64_t)(uintptr_t)page;
}
uint32_t mm_free_pages(uint64_t address, uint64_t count) {
    CHECK(address && count == 1 && core_id == 0 && pages_live);
#ifdef _WIN32
    _aligned_free((void*)(uintptr_t)address);
#else
    free((void*)(uintptr_t)address);
#endif
    pages_live--;
    return 1;
}

static uint64_t kernel_level1[MMU_TABLE_ENTRIES] = {0x1003, 0x2003, 0x3003, 0x4003};
const uint64_t* mmu_kernel_level1(void) { return kernel_level1; }
void mmu_publish_tables(void) {}
static uint32_t flushed[256], flushes, code_syncs;
void mmu_flush_asid(uint32_t asid) {
    CHECK(asid && asid < 256 && core_id == 0);
    /* Teardown must never flush an address space a core still has installed. */
    for (unsigned int core = 0; core < TASK_CORES; core++) CHECK((host_root[core] >> 48) != asid);
    flushed[asid]++;
    flushes++;
}
void mmu_clean_code(uintptr_t start, size_t size) { CHECK(start && size == 4096); }
void mmu_invalidate_code(void) { code_syncs++; }

#ifdef _WIN32
static DWORD WINAPI worker(void* argument) {
#else
static void* worker(void* argument) {
#endif
    core_id = (unsigned int)(uintptr_t)argument;
    parallel_worker_online(core_id);
    while (!parallel_load(&stopping)) {
        if (!parallel_load(&allow_workers)) {
            parallel_store(&parked[core_id], 1);
            parallel_idle();
        } else {
            parallel_store(&parked[core_id], 0);
            if (!parallel_worker_poll(core_id)) parallel_idle();
        }
    }
    return 0;
}

static void wait_flag(const uint32_t* flag) {
    uint32_t start = parallel_time_us();
    while (!parallel_load(flag)) {
        CHECK(parallel_time_us() - start < 10000000U);
        parallel_idle();
    }
}

static void host_start_workers(void) {
    for (unsigned int core = 1; core < TASK_CORES; core++) {
#ifdef _WIN32
        threads[core] = CreateThread(NULL, 0, worker, (void*)(uintptr_t)core, 0, NULL);
        CHECK(threads[core]);
#else
        CHECK(!pthread_create(&threads[core], NULL, worker, (void*)(uintptr_t)core));
#endif
    }
}

static void host_stop_workers(void) {
    parallel_store(&stopping, 1);
    for (unsigned int core = 1; core < TASK_CORES; core++) {
#ifdef _WIN32
        CHECK(WaitForSingleObject(threads[core], 10000) == WAIT_OBJECT_0);
        CloseHandle(threads[core]);
#else
        CHECK(!pthread_join(threads[core], NULL));
#endif
    }
}
