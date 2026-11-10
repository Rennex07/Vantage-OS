#pragma once
#include <vantage/abi/memory.h>

/* Kernel-only API: core 0, EL1, all DAIF exception masks set. Workers receive
 * caller-owned buffers; join all users before freeing. Zero/null means failure.
 * Raw pointers do not detect stale frees after an address has been reallocated.
 */
/* Boot-only and one shot, after boot.s validates assets before BSS clearing.
 * Failure must halt boot. Firmware must supply a readable,
 * immutable DTB. Numeric bounds cannot establish physical pointer readability. */
uint32_t mm_prepare(uintptr_t dtb, uintptr_t kernel_end);
uint32_t mm_finalize_framebuffer(uintptr_t address, uint64_t size);
uint32_t mm_block_mapped(uint64_t block_address);
uint32_t mm_init_allocator(void);
uint32_t mm_self_test(void);

uint64_t mm_alloc_pages(uint64_t count, uint64_t alignment_pages);
uint32_t mm_free_pages(uint64_t address, uint64_t count);
void* mm_alloc(uint64_t size, uint64_t alignment);
void* mm_zalloc(uint64_t size, uint64_t alignment);
uint32_t mm_free(void* address);
/* out is exclusive writable aligned kernel memory, not user memory or metadata. */
uint32_t mm_stats(MemoryStats* out);
#ifdef _MSC_VER
__declspec(noreturn) void mm_halt(const char* message);
#else
void mm_halt(const char* message) __attribute__((noreturn));
#endif
