#include <vantage/kernel/files.h>
#include <vantage/platform/pi4/storage.h>
#include <vantage/platform/pi4/sd.h>
#include <vantage/kernel/task.h>

static int read_sector(void* context, uint64_t lba, void* data) {
    (void)context;
    return sd_read_blocks(lba, 1, data, STORE_SECTOR_BYTES);
}
static int write_sector(void* context, uint64_t lba, const void* data) {
    (void)context;
    return sd_write_blocks(lba, 1, data, STORE_SECTOR_BYTES);
}
static int sync_device(void* context) { (void)context; return sd_sync(); }

int storage_start(uint32_t base_clock_hz, unsigned int partition) {
    if (!task_services_allowed()) return FILE_UNAVAILABLE;
    if (sd_init(base_clock_hz) != SD_OK) return FILE_IO_ERROR;
    SectorDevice device = {NULL, sd_sector_count(), read_sector, write_sector, sync_device};
    return files_mount(&device, partition);
}

int storage_reboot_probe(void) {
    /* Low ID bits 63 cannot identify any scheduler slot. Never retain handles. */
    const uint64_t owner = ~(uint64_t)0;
    unsigned char expected[256], actual[256];
    for (unsigned int i = 0; i < sizeof(expected); i++) expected[i] = (unsigned char)(i * 37U + 0x5aU);
    int64_t handle = files_open(owner, "/ram/.s1-reboot", FILE_READ);
    int initialize = handle == FILE_NOT_FOUND;
    int result = 0;
    if (!initialize) {
        if (handle < 0) return (int)handle;
        result = files_read(owner, (uint64_t)handle, actual, sizeof(actual));
        /* Creation and the first write are separate snapshot commits. An empty
         * probe is the durable intermediate state of interrupted initialization,
         * not proof of a failed persistence check. Never repair nonempty data. */
        initialize = result == 0;
    }
    if (result == (int)sizeof(expected)) {
        if (memcmp(actual, expected, sizeof(expected)) || files_read(owner, (uint64_t)handle, actual, 1) != 0)
            result = FILE_CORRUPT;
    }
    int closed = handle < 0 ? 0 : files_close(owner, (uint64_t)handle);
    if (result < 0) return result;
    if (closed) return closed;
    if (initialize) {
        handle = files_open(owner, "/ram/.s1-reboot", FILE_WRITE | FILE_CREATE);
        if (handle < 0) return (int)handle;
        result = files_write(owner, (uint64_t)handle, expected, sizeof(expected));
        closed = files_close(owner, (uint64_t)handle);
    }
    if (result < 0) return result;
    if (result != (int)sizeof(expected)) return FILE_CORRUPT;
    return closed ? closed : !initialize;
}
