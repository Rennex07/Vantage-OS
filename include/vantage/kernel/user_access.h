#pragma once
#include <vantage/base.h>

/* Syscall access to the CALLING process's memory. The kernel never
 * dereferences a user pointer: it resolves the complete range through that
 * process's translation tables (range and EL0 permission) and copies through
 * its own mapping. A rejected range has no partial effect.
 *
 * On the Pi, a range the process may not access terminates the process with
 * PROCESS_FAULT_ADDRESS and does not return to it; callers that are not
 * process threads get 0. The desktop build has one host address space; host
 * tests of the process layer select the kernel implementation instead. */
#if defined(VANTAGE_SIM) && !defined(VANTAGE_PROCESS_MODEL)
#include <vantage/kernel/platform.h>

static inline uint32_t user_access_ok(uintptr_t address, size_t size, uint32_t write) {
    (void)write;
    return user_buffer_valid(address, size);
}
static inline uint32_t user_copy_in(void* destination, uintptr_t source, size_t size) {
    if (!user_buffer_valid(source, size)) return 0;
    memcpy(destination, (const void*)source, size);
    return 1;
}
static inline uint32_t user_copy_out(uintptr_t destination, const void* source, size_t size) {
    if (!user_buffer_valid(destination, size)) return 0;
    memcpy((void*)destination, source, size);
    return 1;
}
#else
uint32_t user_access_ok(uintptr_t address, size_t size, uint32_t write);
uint32_t user_copy_in(void* destination, uintptr_t source, size_t size);
uint32_t user_copy_out(uintptr_t destination, const void* source, size_t size);
#endif
