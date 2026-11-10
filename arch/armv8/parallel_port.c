#include <vantage/kernel/parallel_port.h>
#include <vantage/platform/pi4/io.h>

uint32_t parallel_load(const uint32_t* value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

void parallel_store(uint32_t* value, uint32_t next) {
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

unsigned int parallel_core_id(void) {
    uint64_t id;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(id));
    return (unsigned int)(id & 0xff);
}

uint32_t parallel_time_us(void) {
    return *(volatile uint32_t*)0xfe003004;
}

void parallel_idle(void) {
    __asm__ volatile("wfe" ::: "memory");
}

void parallel_wake(void) {
    /* Complete publication before signalling, including the check -> WFE race. */
    __asm__ volatile("dsb ishst\nsev" ::: "memory");
}

void parallel_completion_barrier(void) {
    /* Framebuffer rows are noncached and may be consumed outside the CPU
     * cluster. Complete their writes before publishing a cached done flag. */
    __asm__ volatile("dsb sy" ::: "memory");
}

void parallel_port_halt(const char* message) {
    if (parallel_core_id() == 0) uart_writeText(message);
    for (;;) parallel_idle();
}
