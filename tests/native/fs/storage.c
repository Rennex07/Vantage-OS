#include "storage_fixture.h"

static StoredFile image[FILE_LIMIT], recovered[FILE_LIMIT];
static unsigned char baseline[DISK_SECTORS][512];

static void formatted(Store* store) {
    fixture_reset();
    CHECK(store_format(store, &device, &region, STORE_FORMAT_CONFIRM) == 0);
    CHECK(store_mount(store, &device, &region, image) == 0);
}
static void headers_rechecksum(unsigned int bank) {
    unsigned char* header = disk.bytes[region.first + bank * STORE_BANK_SECTORS];
    uint32_t crc = store_crc32(0, header + 512, STORE_PAYLOAD_SECTORS * 512);
    fixture_put32(header + 24, crc);
    fixture_put32(header + 508, store_crc32(0, header, 508));
}
static void partition_checks(void) {
    StoreRegion out = {0, 0};
    fixture_reset();
    CHECK(store_partition(&device, 0, &out) == 0 && out.first == 8 && out.sectors == STORE_MIN_SECTORS);
    CHECK(store_partition(&device, 4, &out) == FILE_BAD);
    CHECK(store_partition(&device, 1, &out) == FILE_BAD);
    disk.bytes[0][510] = 0;
    CHECK(store_partition(&device, 0, &out) == FILE_CORRUPT);
    fixture_reset(); disk.bytes[0][450] = 0x0c;
    CHECK(store_partition(&device, 0, &out) == FILE_BAD);
    fixture_reset(); fixture_put32(disk.bytes[0] + 454, 0);
    CHECK(store_partition(&device, 0, &out) == FILE_CORRUPT);
    fixture_reset(); fixture_put32(disk.bytes[0] + 458, 0xffffffffU);
    CHECK(store_partition(&device, 0, &out) == FILE_CORRUPT);
    fixture_reset(); memcpy(disk.bytes[0] + 462, disk.bytes[0] + 446, 16);
    CHECK(store_partition(&device, 0, &out) == FILE_CORRUPT);
    fixture_reset(); disk.bytes[0][466] = 0xee;
    CHECK(store_partition(&device, 0, &out) == FILE_CORRUPT);
    CHECK(!disk.writes);
}
static void corruption_checks(void) {
    Store store;
    fixture_reset();
    CHECK(store_mount(&store, &device, &region, image) == FILE_CORRUPT && !disk.writes);
    CHECK(store_format(&store, &device, &region, 0) == FILE_BAD && !disk.writes);
    formatted(&store);
    image[0].used = 1; image[0].size = 5; memcpy(image[0].name, "hello", 6); memcpy(image[0].data, "world", 5);
    CHECK(store_commit(&store, image) == 0);
    unsigned int newest = store.active;
    uint64_t first = region.first + newest * STORE_BANK_SECTORS;
    disk.bytes[first + 2][0] ^= 1;
    CHECK(store_mount(&store, &device, &region, recovered) == STORE_MOUNT_RECOVERED && !recovered[0].used);
    disk.bytes[region.first + (newest ^ 1U) * STORE_BANK_SECTORS][0] ^= 1;
    CHECK(store_mount(&store, &device, &region, recovered) == FILE_CORRUPT && !store.state);
    for (unsigned int kind = 0; kind < 5; kind++) {
        formatted(&store);
        unsigned int bank = store.active;
        unsigned char* metadata = disk.bytes[region.first + bank * STORE_BANK_SECTORS + 1];
        memcpy(metadata, "bad", 4); fixture_put32(metadata + 32, 1);
        if (kind == 0) fixture_put32(metadata + 36, FILE_CAPACITY + 1);
        if (kind == 1) metadata[0] = '/';
        if (kind == 2) fixture_put32(metadata + 32, 2);
        if (kind == 3) metadata[40] = 1;
        if (kind == 4) memcpy(metadata + STORE_FILE_SECTORS * 512, metadata, 512);
        headers_rechecksum(bank);
        CHECK(store_mount(&store, &device, &region, recovered) == STORE_MOUNT_RECOVERED);
    }
    formatted(&store); store.generation = ~(uint64_t)0;
    unsigned int writes = disk.writes;
    CHECK(store_commit(&store, image) == FILE_NO_SPACE && disk.writes == writes);
    formatted(&store);
    fixture_put32(disk.bytes[region.first] + 16, 2);
    headers_rechecksum(0);
    CHECK(store_mount(&store, &device, &region, image) == FILE_CORRUPT);
    formatted(&store);
    disk.fail_at = disk.calls + 1;
    CHECK(store_mount(&store, &device, &region, image) == FILE_IO_ERROR && store.state == -1);
    disk.fail_at = 0;
}
static void power_cuts(void) {
    Store store;
    formatted(&store);
    image[0].used = 1; image[0].size = 3;
    memcpy(image[0].name, "note", 5); memcpy(image[0].data, "old", 3);
    CHECK(store_commit(&store, image) == 0);
    memcpy(baseline, disk.bytes, sizeof(baseline));
    memcpy(image[0].data, "new", 3);
    disk.calls = 0;
    CHECK(store_commit(&store, image) == 0);
    unsigned int boundaries = disk.calls;
    for (unsigned int buffered = 0; buffered <= 1; buffered++) {
        for (unsigned int boundary = 1; boundary <= boundaries; boundary++) {
            const unsigned int tears[] = {0, 1, 255, 511, 512};
            for (unsigned int t = 0; t < sizeof(tears) / sizeof(tears[0]); t++) {
                memcpy(disk.bytes, baseline, sizeof(baseline));
                disk.fail_at = 0; disk.buffered = 0;
                CHECK(store_mount(&store, &device, &region, recovered) == 0);
                memcpy(disk.pending, disk.bytes, sizeof(disk.bytes));
                disk.calls = 0; disk.fail_at = boundary; disk.tear = tears[t]; disk.buffered = buffered;
                CHECK(store_commit(&store, image) == FILE_IO_ERROR && store.state == -1);
                CHECK(store_commit(&store, image) == FILE_UNAVAILABLE);
                /* Reboot loses volatile pending writes. Every recoverable state
                 * is exactly the old or new acknowledged-file snapshot. */
                disk.fail_at = 0; disk.buffered = 0;
                int result = store_mount(&store, &device, &region, recovered);
                CHECK(result == 0 || result == STORE_MOUNT_RECOVERED);
                CHECK(recovered[0].used == 1 && recovered[0].size == 3);
                CHECK(!memcmp(recovered[0].data, "old", 3) || !memcmp(recovered[0].data, "new", 3));
                CHECK(!memcmp(disk.bytes[0], baseline[0], 8 * 512));
                CHECK(!memcmp(disk.bytes[region.first + region.sectors], baseline[region.first + region.sectors], 8 * 512));
            }
        }
    }
    printf("PASS: %u I/O cut points, five torn-sector lengths, direct and sync-buffered devices\n", boundaries);
}
int main(void) {
    CHECK(store_crc32(0, "123456789", 9) == 0xcbf43926U);
    CHECK(store_crc32(store_crc32(0, "1234", 4), "56789", 5) == 0xcbf43926U);
    partition_checks(); corruption_checks(); power_cuts();
    return 0;
}
