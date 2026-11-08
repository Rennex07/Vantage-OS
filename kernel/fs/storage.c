#include "../../includes/storage.h"

#if FILE_LIMIT != 16 || FILE_CAPACITY != 4096
#error Storage format version 1 requires 16 files of 4096 bytes
#endif

static const unsigned char magic[8] = {'V', 'S', 'T', 'O', 'R', 'E', '1', 0};

static uint32_t get32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const unsigned char* p) { return get32(p) | ((uint64_t)get32(p + 4) << 32); }
static void put32(unsigned char* p, uint32_t value) {
    for (unsigned int i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (i * 8));
}
static void put64(unsigned char* p, uint64_t value) { put32(p, (uint32_t)value); put32(p + 4, (uint32_t)(value >> 32)); }

uint32_t store_crc32(uint32_t crc, const void* data, size_t bytes) {
    static const uint32_t nibble[16] = {
        0, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU, 0x76dc4190U, 0x6b6b51f4U, 0x4db26158U, 0x5005713cU,
        0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU, 0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU
    };
    const unsigned char* p = data;
    crc = ~crc;
    while (bytes--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ nibble[crc & 15U];
        crc = (crc >> 4) ^ nibble[crc & 15U];
    }
    return ~crc;
}

static int device_valid(const SectorDevice* d) { return d && d->read && d->write && d->sync && d->sectors; }
static int region_valid(const SectorDevice* d, const StoreRegion* r) {
    return device_valid(d) && r && r->first && r->sectors >= STORE_MIN_SECTORS &&
           r->first < d->sectors && r->sectors <= d->sectors - r->first;
}

int store_partition(const SectorDevice* d, unsigned int index, StoreRegion* region) {
    unsigned char sector[STORE_SECTOR_BYTES];
    StoreRegion entries[4] = {{0, 0}};
    if (!device_valid(d) || index > 3 || !region) return FILE_BAD;
    if (d->read(d->context, 0, sector)) return FILE_IO_ERROR;
    if (sector[510] != 0x55 || sector[511] != 0xaa) return FILE_CORRUPT;
    for (unsigned int i = 0; i < 4; i++) {
        const unsigned char* entry = sector + 446 + 16 * i;
        uint32_t type = entry[4];
        entries[i].first = get32(entry + 8); entries[i].sectors = get32(entry + 12);
        if (!type && !entries[i].first && !entries[i].sectors && !entry[0]) continue;
        if (!type || (entry[0] != 0 && entry[0] != 0x80) || type == 0xee ||
            type == 5 || type == 0x0f || type == 0x85 || !entries[i].first || !entries[i].sectors ||
            entries[i].first >= d->sectors || entries[i].sectors > d->sectors - entries[i].first)
            return FILE_CORRUPT;
        for (unsigned int j = 0; j < i; j++)
            if (entries[j].sectors && entries[i].first < entries[j].first + entries[j].sectors &&
                entries[j].first < entries[i].first + entries[i].sectors) return FILE_CORRUPT;
    }
    if (sector[446 + index * 16 + 4] != 0xda || !region_valid(d, &entries[index])) return FILE_BAD;
    *region = entries[index];
    return 0;
}

int store_name_valid(const char name[STORE_NAME_BYTES]) {
    for (unsigned int i = 0; i < STORE_NAME_BYTES; i++) {
        char c = name[i];
        if (!c) return i && !(name[0] == '.' && (i == 1 || (i == 2 && name[1] == '.')));
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) return 0;
    }
    return 0;
}

static int files_valid(const StoredFile files[FILE_LIMIT]) {
    for (unsigned int i = 0; i < FILE_LIMIT; i++) {
        const StoredFile* f = &files[i];
        if (f->used > 1 || f->size > FILE_CAPACITY) return 0;
        if (!f->used) { if (f->size || f->name[0]) return 0; continue; }
        if (!store_name_valid(f->name)) return 0;
        for (unsigned int j = 0; j < i; j++)
            if (files[j].used && !memcmp(f->name, files[j].name, STORE_NAME_BYTES)) return 0;
    }
    return 1;
}

static uint64_t bank_start(const Store* s, unsigned int bank) {
    return s->region.first + (uint64_t)bank * STORE_BANK_SECTORS;
}
static int fault(Store* s) { s->state = -1; return FILE_IO_ERROR; }

/* Canonical encoding has no C layout/padding/endian dependency. Every byte,
 * including unused capacity, is covered by the snapshot checksum. */
static void encode(const StoredFile files[FILE_LIMIT], unsigned int offset, unsigned char sector[512]) {
    const StoredFile* f = &files[offset / STORE_FILE_SECTORS];
    unsigned int part = offset % STORE_FILE_SECTORS;
    memset(sector, 0, 512);
    if (!f->used) return;
    if (!part) {
        memcpy(sector, f->name, STORE_NAME_BYTES); put32(sector + 32, 1); put32(sector + 36, f->size);
    } else {
        unsigned int start = (part - 1) * 512;
        unsigned int count = f->size > start ? f->size - start : 0;
        if (count > 512) count = 512;
        memcpy(sector, f->data + start, count);
    }
}

static int load(Store* s, unsigned int bank, StoredFile files[FILE_LIMIT], uint64_t* generation) {
    unsigned char sector[512];
    uint64_t base = bank_start(s, bank);
    if (s->device.read(s->device.context, base, sector)) return FILE_IO_ERROR;
    if (memcmp(sector, magic, sizeof(magic)) || get32(sector + 8) != 1 || get32(sector + 12) != bank ||
        !get64(sector + 16) || get32(sector + 28) != STORE_PAYLOAD_SECTORS ||
        get64(sector + 32) != s->region.first || get64(sector + 40) != s->region.sectors ||
        get32(sector + 508) != store_crc32(0, sector, 508)) return FILE_CORRUPT;
    for (unsigned int i = 48; i < 508; i++) if (sector[i]) return FILE_CORRUPT;
    uint32_t expected = get32(sector + 24), crc = 0;
    *generation = get64(sector + 16);
    memset(files, 0, sizeof(StoredFile) * FILE_LIMIT);
    for (unsigned int offset = 0; offset < STORE_PAYLOAD_SECTORS; offset++) {
        if (s->device.read(s->device.context, base + 1 + offset, sector)) return FILE_IO_ERROR;
        crc = store_crc32(crc, sector, sizeof(sector));
        StoredFile* f = &files[offset / STORE_FILE_SECTORS];
        unsigned int part = offset % STORE_FILE_SECTORS;
        if (!part) {
            memcpy(f->name, sector, STORE_NAME_BYTES); f->used = get32(sector + 32); f->size = get32(sector + 36);
            for (unsigned int j = 40; j < 512; j++) if (sector[j]) return FILE_CORRUPT;
        } else memcpy(f->data + (part - 1) * 512, sector, 512);
    }
    if (crc != expected || !files_valid(files)) return FILE_CORRUPT;
    /* Reject noncanonical name tails, padding and bytes beyond EOF too. */
    for (unsigned int i = 0; i < FILE_LIMIT; i++) {
        StoredFile* f = &files[i];
        unsigned int end = 0;
        while (end < STORE_NAME_BYTES && f->name[end]) end++;
        for (; end < STORE_NAME_BYTES; end++) if (f->name[end]) return FILE_CORRUPT;
        for (unsigned int j = f->size; j < FILE_CAPACITY; j++) if (f->data[j]) return FILE_CORRUPT;
    }
    return 0;
}

int store_mount(Store* s, const SectorDevice* d, const StoreRegion* region, StoredFile files[FILE_LIMIT]) {
    if (!s || !files || !region_valid(d, region)) return FILE_BAD;
    *s = (Store){0}; s->device = *d; s->region = *region;
    uint64_t generations[2] = {0, 0};
    int status[2];
    for (unsigned int i = 0; i < 2; i++) {
        status[i] = load(s, i, files, &generations[i]);
        if (status[i] == FILE_IO_ERROR) { memset(files, 0, sizeof(StoredFile) * FILE_LIMIT); return fault(s); }
    }
    if ((status[0] && status[1]) || (!status[0] && !status[1] && generations[0] == generations[1])) {
        memset(files, 0, sizeof(StoredFile) * FILE_LIMIT); return FILE_CORRUPT;
    }
    s->active = status[0] ? 1U : status[1] || generations[0] > generations[1] ? 0U : 1U;
    s->generation = generations[s->active];
    int result = load(s, s->active, files, &s->generation);
    if (result) { memset(files, 0, sizeof(StoredFile) * FILE_LIMIT); s->state = -1; return result; }
    s->state = 1;
    return status[0] || status[1] ? STORE_MOUNT_RECOVERED : 0;
}

int store_commit(Store* s, const StoredFile files[FILE_LIMIT]) {
    if (!s || s->state != 1) return FILE_UNAVAILABLE;
    if (!files || !files_valid(files)) return FILE_BAD;
    if (s->generation == ~(uint64_t)0) return FILE_NO_SPACE;
    unsigned int bank = s->active ^ 1U;
    uint64_t base = bank_start(s, bank);
    unsigned char sector[512] = {0};
    if (s->device.write(s->device.context, base, sector) || s->device.sync(s->device.context)) return fault(s);
    uint32_t crc = 0;
    for (unsigned int offset = 0; offset < STORE_PAYLOAD_SECTORS; offset++) {
        encode(files, offset, sector);
        crc = store_crc32(crc, sector, sizeof(sector));
        if (s->device.write(s->device.context, base + 1 + offset, sector)) return fault(s);
    }
    if (s->device.sync(s->device.context)) return fault(s);
    uint32_t readback = 0;
    for (unsigned int offset = 0; offset < STORE_PAYLOAD_SECTORS; offset++) {
        if (s->device.read(s->device.context, base + 1 + offset, sector)) return fault(s);
        readback = store_crc32(readback, sector, sizeof(sector));
    }
    if (readback != crc) return fault(s);
    memset(sector, 0, sizeof(sector)); memcpy(sector, magic, sizeof(magic));
    put32(sector + 8, 1); put32(sector + 12, bank); put64(sector + 16, s->generation + 1);
    put32(sector + 24, crc); put32(sector + 28, STORE_PAYLOAD_SECTORS);
    put64(sector + 32, s->region.first); put64(sector + 40, s->region.sectors);
    put32(sector + 508, store_crc32(0, sector, 508));
    uint32_t header_crc = store_crc32(0, sector, 512);
    if (s->device.write(s->device.context, base, sector) || s->device.sync(s->device.context) ||
        s->device.read(s->device.context, base, sector) || store_crc32(0, sector, 512) != header_crc) return fault(s);
    s->generation++; s->active = bank;
    return 0;
}

int store_format(Store* s, const SectorDevice* d, const StoreRegion* region, uint64_t confirmation) {
    /* No large automatic arrays on task stacks. Formatting uses a zeroed static
     * image and is, like all storage calls, non-reentrant and core-0 only. */
    static const StoredFile empty[FILE_LIMIT] = {{{0}, 0, 0, {0}}};
    if (!s || confirmation != STORE_FORMAT_CONFIRM || !region_valid(d, region)) return FILE_BAD;
    *s = (Store){0}; s->device = *d; s->region = *region;
    unsigned char zero[512] = {0};
    for (unsigned int bank = 0; bank < 2; bank++)
        if (d->write(d->context, bank_start(s, bank), zero)) return fault(s);
    if (d->sync(d->context)) return fault(s);
    s->state = 1; s->active = 1;
    int result = store_commit(s, empty);
    return result ? result : store_commit(s, empty);
}
