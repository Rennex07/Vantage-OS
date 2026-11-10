#define VANTAGE_PROCESS_MODEL
#include "internal.h"
#include <vantage/arch/mmu.h>
#include <vantage/kernel/files.h>
#include <vantage/kernel/parallel_port.h>
#include <vantage/kernel/user_access.h>

#define ID_BITS 6U
#define ID_MASK 63U
/* Keeps process IDs apart from task IDs where both name a resource owner. */
#define PROCESS_TAG (1ULL << 62)
#define GENERATION_LIMIT ((1ULL << 50) - 1)
#define ASID_LIMIT 255U
#define STACK_SPAN 0x10000ULL
#define ACCESS_LIMIT 4096U

typedef char process_ids_must_fit[(PROCESS_LIMIT < ID_MASK) ? 1 : -1];
typedef char every_process_gets_an_asid[(PROCESS_LIMIT < ASID_LIMIT) ? 1 : -1];
typedef char stacks_leave_a_guard[(PROCESS_STACK_BYTES < STACK_SPAN) ? 1 : -1];

static Process processes[PROCESS_LIMIT];
/* ASID 0 is the kernel-only root. An ASID belongs to one process from creation
 * until its translations are flushed on every core at teardown, so the
 * next-fit cursor may wrap and hand it out again. Scheduler lock. */
static unsigned char asid_used[ASID_LIMIT + 1U];
static uint32_t asid_cursor;

static uint32_t asid_take(void) {
    for (unsigned int tried = 0; tried < ASID_LIMIT; tried++) {
        uint32_t asid = asid_cursor % ASID_LIMIT + 1U;
        asid_cursor = asid % ASID_LIMIT;
        if (!asid_used[asid]) {
            asid_used[asid] = 1;
            return asid;
        }
    }
    return 0;
}

/* Core 0 in boot or core-0-pinned kernel context: the page allocator's owner. */
static int coordinator(void) {
    if (!task_services_allowed()) return 0;
    Task* task = sched_running();
    return !task || !task->process;
}

Process* process_lookup(ProcessId id) {
    unsigned int slot = (unsigned int)(id & ID_MASK);
    if (!slot || slot > PROCESS_LIMIT) return NULL;
    Process* process = &processes[slot - 1];
    return process->id == id && (process->state == PROCESS_LIVE || process->state == PROCESS_EXITED)
        ? process : NULL;
}

void process_thread_exited(Process* process) {
    if (!--process->live) process->state = PROCESS_EXITED;
}

static void thread_start(void* argument) {
    const ProcessThread* thread = argument;
    task_port_enter_user(thread->entry, thread->stack, thread->argument);
}

static TaskId add_thread(Process* process, uint64_t entry, uint64_t argument, uint32_t affinity) {
    unsigned int index = process->threads;
    if (index >= PROCESS_THREADS) return 0;
    ProcessThread* thread = &process->thread[index];
    thread->task = 0;
    thread->entry = entry;
    thread->argument = argument;
    thread->stack = USER_BASE + USER_BYTES - index * STACK_SPAN;
    /* The slot stays consumed on failure: a partly mapped stack cannot be
     * withdrawn from tables another core may already have walked. */
    sched_lock();
    process->threads = index + 1U;
    sched_unlock();
    if (!space_map(&process->space, thread->stack - PROCESS_STACK_BYTES,
                   PROCESS_STACK_BYTES / USER_PAGE_BYTES, SPACE_READ | SPACE_WRITE)) return 0;
    TaskId id = sched_spawn(thread_start, thread, affinity, process);
    sched_lock();
    thread->task = id;
    sched_unlock();
    return id;
}

static int image_valid(const ProcessImage* image, size_t bytes) {
    if (bytes < sizeof(*image) || image->magic != PROCESS_IMAGE_MAGIC) return 0;
    if (image->text_bytes > PROCESS_IMAGE_LIMIT || image->rodata_bytes > PROCESS_IMAGE_LIMIT ||
        image->data_bytes > PROCESS_IMAGE_LIMIT || image->bss_bytes > PROCESS_IMAGE_LIMIT) return 0;
    if (!image->text_bytes || (image->text_bytes | image->rodata_bytes) % USER_PAGE_BYTES) return 0;
    if (image->text_bytes + image->rodata_bytes + image->data_bytes != bytes) return 0;
    return image->entry >= USER_BASE + sizeof(*image) && !(image->entry & 3U) &&
           image->entry - USER_BASE < image->text_bytes;
}

static int load(Process* process, const ProcessImage* image) {
    AddressSpace* space = &process->space;
    const unsigned char* file = (const unsigned char*)image;
    uint64_t rodata = USER_BASE + image->text_bytes, data = rodata + image->rodata_bytes;
    uint64_t writable = (image->data_bytes + image->bss_bytes + USER_PAGE_BYTES - 1) / USER_PAGE_BYTES;
    if (!space_map(space, USER_BASE, image->text_bytes / USER_PAGE_BYTES, SPACE_READ | SPACE_EXECUTE) ||
        !space_load(space, USER_BASE, file, (size_t)image->text_bytes)) return 0;
    if (image->rodata_bytes &&
        (!space_map(space, rodata, image->rodata_bytes / USER_PAGE_BYTES, SPACE_READ) ||
         !space_load(space, rodata, file + image->text_bytes, (size_t)image->rodata_bytes))) return 0;
    if (writable && !space_map(space, data, writable, SPACE_READ | SPACE_WRITE)) return 0;
    if (image->data_bytes &&
        !space_load(space, data, file + image->text_bytes + image->rodata_bytes,
                    (size_t)image->data_bytes)) return 0;
    space_sync_code(space, USER_BASE, image->text_bytes / USER_PAGE_BYTES);
    return 1;
}

/* Core 0. No thread exists and no core has the root installed. */
static void discard(Process* process) {
    mmu_flush_asid(process->asid);
    space_destroy(&process->space);
    sched_lock();
    asid_used[process->asid] = 0;
    process->state = PROCESS_FREE;
    sched_unlock();
}

ProcessId process_create(const void* image, size_t bytes, uint32_t affinity) {
    const ProcessImage* header = image;
    if (!coordinator() || !image || ((uintptr_t)image & 7U) ||
        !image_valid(header, bytes)) return 0;
    Process* process = NULL;
    sched_lock();
    for (unsigned int i = 0; i < PROCESS_LIMIT && !process; i++) {
        if (processes[i].state != PROCESS_FREE || processes[i].generation == GENERATION_LIMIT) continue;
        uint32_t asid = asid_take();
        if (!asid) break;
        process = &processes[i];
        process->generation++;
        process->id = PROCESS_TAG | (process->generation << ID_BITS) | (i + 1U);
        process->state = PROCESS_CHANGING;
        process->status = 0;
        process->terminating = 0;
        process->threads = process->live = 0;
        process->asid = asid;
    }
    sched_unlock();
    if (!process) return 0;
    if (!space_create(&process->space, mmu_kernel_level1())) {
        sched_lock();
        asid_used[process->asid] = 0;
        process->state = PROCESS_FREE;
        sched_unlock();
        return 0;
    }
    if (load(process, header)) {
        sched_lock();
        process->root = space_root(&process->space, process->asid);
        process->state = PROCESS_LIVE;
        sched_unlock();
        if (add_thread(process, header->entry, 0, affinity)) return process->id;
        sched_lock();
        process->state = PROCESS_CHANGING;
        sched_unlock();
    }
    discard(process);
    return 0;
}

TaskId process_thread(ProcessId id, uint64_t entry, uint64_t argument, uint32_t affinity) {
    if (!coordinator()) return 0;
    sched_lock();
    Process* process = process_lookup(id);
    int usable = process && process->state == PROCESS_LIVE && !process->terminating;
    sched_unlock();
    if (!usable || (entry & 3U) ||
        !space_accessible(&process->space, entry, 4, SPACE_EXECUTE)) return 0;
    return add_thread(process, entry, argument, affinity);
}

int process_reap(ProcessId id, int* status) {
    if (!coordinator()) return TASK_INVALID;
    sched_lock();
    Process* process = process_lookup(id);
    if (!process) { sched_unlock(); return TASK_INVALID; }
    if (process->state != PROCESS_EXITED) { sched_unlock(); return TASK_PENDING; }
    /* Every thread is exited: its context is saved, it is off its stack and
     * its core has dropped this address space. */
    for (unsigned int i = 0; i < process->threads; i++) {
        Task* task = sched_lookup(process->thread[i].task);
        if (task) sched_release(task);
    }
    process->state = PROCESS_CHANGING;
    if (status) *status = process->status;
    sched_unlock();
    files_release(process->id);
    discard(process);
    return TASK_OK;
}

int process_wait(ProcessId id, int* status) {
    for (;;) {
        int result = process_reap(id, status);
        if (result != TASK_PENDING || task_yield() != TASK_OK) return result;
    }
}

int process_kill(ProcessId id, int status) {
    if (!task_port_owned()) return TASK_INVALID;
    sched_lock();
    Process* process = process_lookup(id);
    if (process && !process->terminating) {
        process->status = status;
        parallel_store(&process->terminating, 1);
    }
    sched_unlock();
    if (process) parallel_wake();
    return process ? TASK_OK : TASK_INVALID;
}

void process_fault(int status) {
    Task* task = sched_running();
    if (task && task->process) task_exit(status);
}

ProcessId process_current(void) {
    Task* task = sched_running();
    return task && task->process ? task->process->id : 0;
}

int process_info(ProcessId id, ProcessInfo* info) {
    if (!task_port_owned() || !info) return TASK_INVALID;
    sched_lock();
    Process* process = process_lookup(id);
    if (process) {
        info->id = process->id;
        info->state = process->state;
        info->status = process->status;
        info->threads = process->threads;
        info->live = process->live;
        info->asid = process->asid;
        info->pages = process->space.pages;
    }
    sched_unlock();
    return process ? TASK_OK : TASK_INVALID;
}

static const AddressSpace* calling_space(void) {
    Task* task = sched_running();
    return task && task->process ? &task->process->space : NULL;
}

uint32_t user_access_ok(uintptr_t address, size_t size, uint32_t write) {
    const AddressSpace* space = calling_space();
    if (!space || size > ACCESS_LIMIT) return 0;
    if (space_accessible(space, address, size, write ? SPACE_WRITE : SPACE_READ)) return 1;
    process_fault(PROCESS_FAULT_ADDRESS);
    return 0;
}

uint32_t user_copy_in(void* destination, uintptr_t source, size_t size) {
    const AddressSpace* space = calling_space();
    if (!space || size > ACCESS_LIMIT) return 0;
    if (space_copy_in(space, destination, source, size)) return 1;
    process_fault(PROCESS_FAULT_ADDRESS);
    return 0;
}

uint32_t user_copy_out(uintptr_t destination, const void* source, size_t size) {
    const AddressSpace* space = calling_space();
    if (!space || size > ACCESS_LIMIT) return 0;
    if (space_copy_out(space, destination, source, size)) return 1;
    process_fault(PROCESS_FAULT_ADDRESS);
    return 0;
}
