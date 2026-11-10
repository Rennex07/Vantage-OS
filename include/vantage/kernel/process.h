#pragma once
#include <vantage/kernel/task.h>

/* A process owns one isolated address space, up to PROCESS_THREADS scheduler
 * tasks running at EL0 inside it, its file handles and an exit status. */
#define PROCESS_LIMIT 8U
#define PROCESS_THREADS 4U
#define PROCESS_IMAGE_MAGIC 0x31676d4945544e56ULL /* "VNTEImg1" */
#define PROCESS_IMAGE_LIMIT (16U * 1024U * 1024U)
#define PROCESS_STACK_BYTES 16384U
typedef uint64_t ProcessId;

/* Flat program image. The text (starting with this header), read-only data
 * and writable data follow each other in the file and in memory from
 * USER_BASE; text_bytes and rodata_bytes are page multiples, zeroed bss
 * follows the data. Text is read/execute, rodata read-only, data read/write. */
typedef struct {
    uint64_t magic;
    uint64_t entry;
    uint64_t text_bytes, rodata_bytes, data_bytes, bss_bytes;
} ProcessImage;

/* Exit statuses of processes terminated by the kernel. */
enum ProcessFault {
    PROCESS_FAULT_DATA = -1001,        /* Data access the address space forbids. */
    PROCESS_FAULT_INSTRUCTION = -1002, /* Instruction fetch it forbids. */
    PROCESS_FAULT_UNDEFINED = -1003,   /* Undefined or privileged instruction. */
    PROCESS_FAULT_ADDRESS = -1004,     /* Syscall buffer outside the process. */
    PROCESS_FAULT_OTHER = -1005        /* Any other synchronous exception. */
};

enum ProcessState {
    PROCESS_FREE,
    PROCESS_CHANGING, /* Being built or torn down on core 0; not addressable. */
    PROCESS_LIVE,
    PROCESS_EXITED    /* Every thread is off its core; waiting to be reaped. */
};

/* Creation, thread creation and reaping allocate or free pages: core 0 only,
 * from boot or a core-0-pinned kernel task, like the page allocator itself.
 * The image is copied; affinity follows task_create_affinity. Zero on failure. */
ProcessId process_create(const void* image, size_t bytes, uint32_t affinity);
/* Adds a thread with its own user stack, entered as entry(argument) at EL0. */
TaskId process_thread(ProcessId id, uint64_t entry, uint64_t argument, uint32_t affinity);
/* Nonblocking. TASK_PENDING while a thread is still on a core or runnable.
 * Success releases file handles, flushes the address space's translations on
 * every core, frees all of its memory and invalidates the ID. */
int process_reap(ProcessId id, int* status);
/* Reaps, yielding the calling kernel task until the process has exited. */
int process_wait(ProcessId id, int* status);

/* Any core, masked EL1. The first termination request fixes the status.
 * Threads stop at their next kernel entry or dispatch, at most one time slice
 * later; they never return to EL0 after that. */
int process_kill(ProcessId id, int status);
/* Terminates the calling thread's process; returns only for kernel callers. */
void process_fault(int status);
ProcessId process_current(void);

typedef struct {
    ProcessId id;
    enum ProcessState state;
    int status;
    uint32_t threads, live, asid;
    uint64_t pages;
} ProcessInfo;
int process_info(ProcessId id, ProcessInfo* info);
