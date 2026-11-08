#pragma once
#include "../../includes/storage.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define DISK_SECTORS (STORE_MIN_SECTORS + 16U)
typedef struct {
    unsigned char bytes[DISK_SECTORS][512], pending[DISK_SECTORS][512];
    unsigned int calls, fail_at, tear, buffered, writes, syncs;
} TestDisk;
static TestDisk disk;
static StoreRegion region = {8, STORE_MIN_SECTORS};

static void fixture_put32(unsigned char* p, uint32_t n) {
    for (unsigned int i = 0; i < 4; i++) p[i] = (unsigned char)(n >> (8 * i));
}
static int disk_read(void* context, uint64_t lba, void* data) {
    TestDisk* d = context;
    CHECK(lba < DISK_SECTORS);
    if (++d->calls == d->fail_at) return -1;
    memcpy(data, d->bytes[lba], 512);
    return 0;
}
static int disk_write(void* context, uint64_t lba, const void* data) {
    TestDisk* d = context;
    CHECK(lba >= region.first && lba < region.first + region.sectors);
    unsigned char* target = d->buffered ? d->pending[lba] : d->bytes[lba];
    d->writes++;
    if (++d->calls == d->fail_at) { memcpy(target, data, d->tear); return -1; }
    memcpy(target, data, 512);
    return 0;
}
static int disk_sync(void* context) {
    TestDisk* d = context;
    d->syncs++;
    if (++d->calls == d->fail_at) return -1;
    if (d->buffered) memcpy(d->bytes, d->pending, sizeof(d->bytes));
    return 0;
}
static SectorDevice device = {&disk, DISK_SECTORS, disk_read, disk_write, disk_sync};
static void fixture_reset(void) {
    memset(&disk, 0, sizeof(disk));
    memset(disk.bytes, 0xa5, sizeof(disk.bytes));
    memset(disk.bytes[0], 0, 512);
    disk.bytes[0][510] = 0x55; disk.bytes[0][511] = 0xaa;
    unsigned char* entry = disk.bytes[0] + 446;
    entry[4] = 0xda; fixture_put32(entry + 8, (uint32_t)region.first);
    fixture_put32(entry + 12, (uint32_t)region.sectors);
    memcpy(disk.pending, disk.bytes, sizeof(disk.bytes));
}
