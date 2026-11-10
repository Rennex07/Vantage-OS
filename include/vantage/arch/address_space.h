#pragma once
#include <vantage/base.h>

/* One process's translation tables. User memory is the gigabyte above the
 * 4 GiB kernel identity map, built from 4 KiB pages owned by the space. */
#define USER_BASE 0x100000000ULL
#define USER_BYTES 0x40000000ULL
#define USER_PAGE_BYTES 4096ULL

#define SPACE_READ 1U
#define SPACE_WRITE 2U
#define SPACE_EXECUTE 4U

typedef struct {
    uint64_t* root;   /* Level 1: shared kernel entries plus the user gigabyte. */
    uint64_t* user;   /* Level 2 of the user gigabyte. */
    uint64_t pages;   /* Pages owned, including the tables. */
} AddressSpace;

/* Single owner: these use the page allocator and so run on core 0 in masked
 * EL1 kernel context. Tables only gain entries while the process lives; other
 * cores may walk them concurrently. A failed create leaves nothing allocated;
 * after a failed map the caller destroys the space. */
uint32_t space_create(AddressSpace* space, const uint64_t kernel_level1[4]);
/* Maps zeroed pages. access is SPACE_READ, optionally with exactly one of
 * SPACE_WRITE or SPACE_EXECUTE; the range must be unmapped user memory. */
uint32_t space_map(AddressSpace* space, uint64_t address, uint64_t pages, uint32_t access);
/* Loader write through the kernel alias, regardless of user permissions. */
uint32_t space_load(const AddressSpace* space, uint64_t address, const void* source, size_t size);
/* After loading executable pages and before any thread runs them. */
void space_sync_code(const AddressSpace* space, uint64_t address, uint64_t pages);
/* Frees every page. No core may still have the root installed and its ASID
 * must already be flushed. */
void space_destroy(AddressSpace* space);
uint64_t space_root(const AddressSpace* space, uint32_t asid);

/* Any core. The complete range must be mapped for EL0 with the access. */
uint32_t space_accessible(const AddressSpace* space, uint64_t address, size_t size, uint32_t access);
uint32_t space_copy_in(const AddressSpace* space, void* destination, uint64_t source, size_t size);
uint32_t space_copy_out(const AddressSpace* space, uint64_t destination, const void* source, size_t size);
