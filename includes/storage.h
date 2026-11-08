#pragma once
#include "files.h"

#define STORE_SECTOR_BYTES 512U
#define STORE_NAME_BYTES 32U
#define STORE_FILE_SECTORS 9U
#define STORE_PAYLOAD_SECTORS (FILE_LIMIT * STORE_FILE_SECTORS)
#define STORE_BANK_SECTORS (STORE_PAYLOAD_SECTORS + 1U)
#define STORE_MIN_SECTORS (2U * STORE_BANK_SECTORS)
#define STORE_MOUNT_RECOVERED 1
#define STORE_FORMAT_CONFIRM 0x56414e5441474531ULL

/* Synchronous, exclusively owned 512-byte sector device. Zero means success.
 * sync must complete preceding writes to the device's durable-storage boundary.
 * No optional volatile write cache may be enabled without flush support. */
typedef struct {
    void* context;
    uint64_t sectors;
    int (*read)(void*, uint64_t, void*);
    int (*write)(void*, uint64_t, const void*);
    int (*sync)(void*);
} SectorDevice;

typedef struct { uint64_t first, sectors; } StoreRegion;
typedef struct {
    char name[STORE_NAME_BYTES];
    uint32_t used, size;
    unsigned char data[FILE_CAPACITY];
} StoredFile;
typedef struct {
    SectorDevice device;
    StoreRegion region;
    uint64_t generation;
    unsigned int active;
    int state;
} Store;

/* Explicit primary MBR partition index, type 0xda, no overlapping/extended/GPT
 * entries. The boot sector and all other partitions remain outside the volume. */
int store_partition(const SectorDevice* device, unsigned int index, StoreRegion* region);
int store_mount(Store* store, const SectorDevice* device, const StoreRegion* region,
                 StoredFile files[FILE_LIMIT]);
int store_commit(Store* store, const StoredFile files[FILE_LIMIT]);
/* Destructive provisioning API, never called by the kernel boot path. */
int store_format(Store* store, const SectorDevice* device, const StoreRegion* region,
                  uint64_t confirmation);
int store_name_valid(const char name[STORE_NAME_BYTES]);
uint32_t store_crc32(uint32_t crc, const void* data, size_t bytes);

/* Hardware validation entry only; normal boot does not initialize EMMC2. */
int storage_start(uint32_t base_clock_hz, unsigned int partition);
/* Validation volume only: create a deterministic probe (0), or check an
 * existing probe (1). A match only proves a reboot if separate logs show one. */
int storage_reboot_probe(void);
