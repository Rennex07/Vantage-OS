#include "../../includes/task_internal.h"
#include "../../includes/io.h"

typedef char context_size_must_match_assembly[(sizeof(TaskContext) == 208) ? 1 : -1];
typedef char context_sp_offset_must_match[( __builtin_offsetof(TaskContext, sp) == 96) ? 1 : -1];
typedef char context_fp_offset_must_match[( __builtin_offsetof(TaskContext, d8_d15) == 104) ? 1 : -1];
typedef char context_elr_offset_must_match[( __builtin_offsetof(TaskContext, elr) == 184) ? 1 : -1];

int task_port_owned(void) {
    uint64_t core, level, mask;
    __asm__ volatile("mrs %0, mpidr_el1\nmrs %1, CurrentEL\nmrs %2, daif"
                     : "=r"(core), "=r"(level), "=r"(mask));
    return (core & 0xff) < TASK_CORES && level == 4 && (mask & 0x3c0) == 0x3c0;
}

unsigned int task_port_core(void) {
    uint64_t core;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(core));
    return (unsigned int)(core & 0xff);
}

void task_port_lock(uint32_t* lock) {
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("yield" ::: "memory");
}

void task_port_unlock(uint32_t* lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

void task_context_init(TaskContext* context, void* stack, size_t size,
                       void (*entry)(void)) {
    *context = (TaskContext){0};
    context->sp = (uintptr_t)stack + size;
    context->x19_x30[11] = (uintptr_t)entry;
}

void task_context_dispose(TaskContext* context) {
    (void)context;
}

void task_port_halt(const char* message) {
    uint64_t core;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(core));
    if (!(core & 0xff)) uart_writeText(message);
    for (;;) __asm__ volatile("wfe" ::: "memory");
}
