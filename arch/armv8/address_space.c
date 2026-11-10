#include <vantage/arch/address_space.h>
#include <vantage/arch/mmu.h>
#include <vantage/kernel/memory.h>

#define ENTRIES 512U
#define VALID 1ULL
#define TABLE 3ULL
#define PAGE 3ULL
#define ATTR_NORMAL (1ULL << 2)
#define AP_EL0 (1ULL << 6)
#define AP_READ_ONLY (1ULL << 7)
#define SHARED_INNER (3ULL << 8)
#define ACCESSED (1ULL << 10)
#define NOT_GLOBAL (1ULL << 11)
#define PXN (1ULL << 53)
#define UXN (1ULL << 54)
#define TABLE_PXN (1ULL << 59)
#define ADDRESS_MASK 0x0000fffffffff000ULL
#define USER_SLOT (USER_BASE >> 30)

typedef char user_gigabyte_follows_kernel[(USER_SLOT == MMU_KERNEL_ENTRIES) ? 1 : -1];

static uint64_t* table_of(uint64_t descriptor) {
    return (uint64_t*)(uintptr_t)(descriptor & ADDRESS_MASK);
}

static uint64_t* new_page(AddressSpace* space) {
    void* page = (void*)(uintptr_t)mm_alloc_pages(1, 1);
    if (!page) return NULL;
    memset(page, 0, USER_PAGE_BYTES);
    space->pages++;
    return page;
}

static void free_page(AddressSpace* space, void* page) {
    if (mm_free_pages((uint64_t)(uintptr_t)page, 1)) space->pages--;
}

static int in_user_range(uint64_t address, uint64_t size) {
    return address >= USER_BASE && address - USER_BASE < USER_BYTES &&
           size <= USER_BYTES - (address - USER_BASE);
}

static uint64_t* leaf(const AddressSpace* space, uint64_t address) {
    uint64_t middle = space->user[(address >> 21) & (ENTRIES - 1)];
    if (!(middle & VALID)) return NULL;
    return &table_of(middle)[(address >> 12) & (ENTRIES - 1)];
}

uint32_t space_create(AddressSpace* space, const uint64_t kernel_level1[4]) {
    if (!space || !kernel_level1) return 0;
    space->pages = 0;
    space->root = new_page(space);
    space->user = space->root ? new_page(space) : NULL;
    if (!space->user) {
        if (space->root) free_page(space, space->root);
        space->root = NULL;
        return 0;
    }
    for (unsigned int i = 0; i < MMU_KERNEL_ENTRIES; i++) space->root[i] = kernel_level1[i];
    space->root[USER_SLOT] = (uint64_t)(uintptr_t)space->user | TABLE | TABLE_PXN;
    mmu_publish_tables();
    return 1;
}

uint32_t space_map(AddressSpace* space, uint64_t address, uint64_t pages, uint32_t access) {
    if (!space || !space->user || !pages || (address & (USER_PAGE_BYTES - 1)) ||
        pages > USER_BYTES / USER_PAGE_BYTES || !in_user_range(address, pages * USER_PAGE_BYTES) ||
        !(access & SPACE_READ) || (access & ~(SPACE_READ | SPACE_WRITE | SPACE_EXECUTE)) ||
        ((access & SPACE_WRITE) && (access & SPACE_EXECUTE))) return 0;
    uint64_t attributes = PAGE | ATTR_NORMAL | SHARED_INNER | ACCESSED | NOT_GLOBAL | AP_EL0 | PXN;
    if (!(access & SPACE_WRITE)) attributes |= AP_READ_ONLY;
    if (!(access & SPACE_EXECUTE)) attributes |= UXN;
    for (uint64_t i = 0; i < pages; i++, address += USER_PAGE_BYTES) {
        uint64_t* middle = &space->user[(address >> 21) & (ENTRIES - 1)];
        if (!(*middle & VALID)) {
            uint64_t* table = new_page(space);
            if (!table) return 0;
            *middle = (uint64_t)(uintptr_t)table | TABLE;
        }
        uint64_t* entry = &table_of(*middle)[(address >> 12) & (ENTRIES - 1)];
        if (*entry & VALID) return 0;
        uint64_t* page = new_page(space);
        if (!page) return 0;
        *entry = (uint64_t)(uintptr_t)page | attributes;
    }
    mmu_publish_tables();
    return 1;
}

void space_destroy(AddressSpace* space) {
    if (!space || !space->root) return;
    for (unsigned int i = 0; i < ENTRIES; i++) {
        if (!(space->user[i] & VALID)) continue;
        uint64_t* table = table_of(space->user[i]);
        for (unsigned int j = 0; j < ENTRIES; j++)
            if (table[j] & VALID) free_page(space, table_of(table[j]));
        free_page(space, table);
    }
    free_page(space, space->user);
    free_page(space, space->root);
    space->root = space->user = NULL;
}

uint64_t space_root(const AddressSpace* space, uint32_t asid) {
    return (uint64_t)(uintptr_t)space->root | ((uint64_t)asid << 48);
}

/* Kernel alias of one user byte, or NULL without the requested EL0 access. */
static unsigned char* resolve(const AddressSpace* space, uint64_t address, uint32_t access) {
    if (!in_user_range(address, 1)) return NULL;
    const uint64_t* entry = leaf(space, address);
    if (!entry || !(*entry & VALID) || !(*entry & AP_EL0)) return NULL;
    if ((access & SPACE_WRITE) && (*entry & AP_READ_ONLY)) return NULL;
    if ((access & SPACE_EXECUTE) && (*entry & UXN)) return NULL;
    return (unsigned char*)table_of(*entry) + (address & (USER_PAGE_BYTES - 1));
}

/* direction: 0 checks only, 1 copies user to kernel, 2 copies kernel to user. */
static uint32_t transfer(const AddressSpace* space, uint64_t address, unsigned char* kernel,
                         size_t size, uint32_t access, int direction) {
    if (!space || !space->user || !size || !in_user_range(address, size)) return 0;
    /* Validate the whole range first: a rejected copy has no partial effect. */
    for (int pass = 0; pass < 2; pass++) {
        uint64_t cursor = address;
        size_t done = 0;
        while (done < size) {
            size_t chunk = (size_t)(USER_PAGE_BYTES - (cursor & (USER_PAGE_BYTES - 1)));
            if (chunk > size - done) chunk = size - done;
            unsigned char* alias = resolve(space, cursor, access);
            if (!alias) return 0;
            if (pass && direction == 1) memcpy(kernel + done, alias, chunk);
            if (pass && direction == 2) memcpy(alias, kernel + done, chunk);
            cursor += chunk;
            done += chunk;
        }
        if (!direction) break;
    }
    return 1;
}

uint32_t space_accessible(const AddressSpace* space, uint64_t address, size_t size, uint32_t access) {
    return transfer(space, address, NULL, size, access, 0);
}

uint32_t space_copy_in(const AddressSpace* space, void* destination, uint64_t source, size_t size) {
    return destination && transfer(space, source, destination, size, SPACE_READ, 1);
}

uint32_t space_copy_out(const AddressSpace* space, uint64_t destination, const void* source, size_t size) {
    return source && transfer(space, destination, (unsigned char*)(uintptr_t)source, size, SPACE_WRITE, 2);
}

void space_sync_code(const AddressSpace* space, uint64_t address, uint64_t pages) {
    for (uint64_t i = 0; i < pages; i++) {
        unsigned char* alias = resolve(space, address + i * USER_PAGE_BYTES, SPACE_EXECUTE);
        if (alias) mmu_clean_code((uintptr_t)alias, USER_PAGE_BYTES);
    }
    mmu_invalidate_code();
}

uint32_t space_load(const AddressSpace* space, uint64_t address, const void* source, size_t size) {
    return source && transfer(space, address, (unsigned char*)(uintptr_t)source, size, 0, 2);
}
