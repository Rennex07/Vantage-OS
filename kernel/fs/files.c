#include "../../includes/files_internal.h"
#ifndef VANTAGE_SIM
#include "../../includes/task.h"
#endif

static int services_owned(void) {
#ifdef VANTAGE_SIM
    return 1;
#else
    return task_services_allowed();
#endif
}

#define NAME_MAX 32U
#define HANDLE_BITS 5U
#define HANDLE_MASK 31U

typedef struct {
    uint64_t owner, id, generation;
    uint32_t file, offset, flags;
} OpenFile;

static StoredFile stored_files[FILE_LIMIT];
static Store volume;
static OpenFile open_files[FILE_HANDLE_LIMIT];
static const char prefix[] = "/ram/";

static int have_handles(void) {
    for (unsigned int i = 0; i < FILE_HANDLE_LIMIT; i++) if (open_files[i].owner) return 1;
    return 0;
}

int files_unmount(void) {
    if (!services_owned()) return FILE_UNAVAILABLE;
    if (have_handles()) return FILE_BUSY;
    volume.state = 0;
    memset(stored_files, 0, sizeof(stored_files));
    return 0;
}

int files_mount(const SectorDevice* device, unsigned int partition) {
    if (!services_owned()) return FILE_UNAVAILABLE;
    if (volume.state == 1 || have_handles()) return FILE_BUSY;
    StoreRegion region;
    int result = store_partition(device, partition, &region);
    if (result) return result;
    return store_mount(&volume, device, &region, stored_files);
}

static int commit(void) {
    int result = store_commit(&volume, stored_files);
    /* Even a rejected commit must not expose staged changes as durable files. */
    if (result) volume.state = -1;
    return result;
}

static int name_from_path(const char* path, char name[NAME_MAX]) {
    if (!path) return 0;
    for (unsigned int i = 0; i < 5; i++) if (path[i] != prefix[i]) return 0;
    for (unsigned int i = 0; i < NAME_MAX; i++) {
        char c = path[i + 5];
        name[i] = c;
        if (!c) return i && !(name[0] == '.' &&
                             (i == 1 || (i == 2 && name[1] == '.')));
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return 0;
    }
    return 0;
}

static int same_name(const char* a, const char* b) {
    for (unsigned int i = 0; i < NAME_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 0;
}

static OpenFile* lookup(uint64_t owner, uint64_t handle) {
    unsigned int slot = (unsigned int)(handle & HANDLE_MASK);
    if (!owner || !slot || slot > FILE_HANDLE_LIMIT) return NULL;
    OpenFile* open = &open_files[slot - 1];
    return open->owner == owner && open->id == handle ? open : NULL;
}

int64_t files_open(uint64_t owner, const char* path, uint32_t flags) {
    char name[NAME_MAX] = {0};
    if (!services_owned() || !owner || volume.state != 1) return FILE_UNAVAILABLE;
    if (!name_from_path(path, name) || !(flags & (FILE_READ | FILE_WRITE)) ||
        (flags & ~(FILE_READ | FILE_WRITE | FILE_CREATE | FILE_TRUNCATE)) ||
        ((flags & (FILE_CREATE | FILE_TRUNCATE)) && !(flags & FILE_WRITE))) return FILE_BAD;
    OpenFile* open = NULL;
    for (unsigned int i = 0; i < FILE_HANDLE_LIMIT; i++) {
        if (!open_files[i].owner && open_files[i].generation < (0x7fffffffffffffffULL >> HANDLE_BITS)) {
            open = &open_files[i];
            break;
        }
    }
    /* Reserve capacity before creating/truncating anything. */
    if (!open) return FILE_NO_SPACE;
    unsigned int existing = FILE_LIMIT, free_slot = FILE_LIMIT;
    for (unsigned int i = 0; i < FILE_LIMIT; i++) {
        if (stored_files[i].used && same_name(stored_files[i].name, name)) existing = i;
        if (!stored_files[i].used && free_slot == FILE_LIMIT) free_slot = i;
    }
    int changed = 0;
    if (existing == FILE_LIMIT) {
        if (!(flags & FILE_CREATE)) return FILE_NOT_FOUND;
        if (free_slot == FILE_LIMIT) return FILE_NO_SPACE;
        existing = free_slot;
        stored_files[existing] = (StoredFile){0};
        memcpy(stored_files[existing].name, name, NAME_MAX);
        stored_files[existing].used = 1;
        changed = 1;
    }
    StoredFile* file = &stored_files[existing];
    if (flags & FILE_TRUNCATE) {
        file->size = 0;
        memset(file->data, 0, FILE_CAPACITY);
        changed = 1;
    }
    if (changed) { int result = commit(); if (result) return result; }
    open->generation++;
    open->id = (open->generation << HANDLE_BITS) | ((unsigned int)(open - open_files) + 1U);
    open->owner = owner;
    open->file = existing;
    open->offset = 0;
    open->flags = flags;
    return (int64_t)open->id;
}

int files_read(uint64_t owner, uint64_t handle, void* data, uint32_t length) {
    if (!services_owned() || volume.state != 1) return FILE_UNAVAILABLE;
    OpenFile* open = lookup(owner, handle);
    if (!open) return FILE_BAD_HANDLE;
    if (!(open->flags & FILE_READ)) return FILE_ACCESS;
    if (length && !data) return FILE_BAD;
    StoredFile* file = &stored_files[open->file];
    uint32_t available = open->offset < file->size ? file->size - open->offset : 0;
    if (length > available) length = available;
    if (length) memcpy(data, file->data + open->offset, length);
    open->offset += length;
    return (int)length;
}

int files_write(uint64_t owner, uint64_t handle, const void* data, uint32_t length) {
    if (!services_owned() || volume.state != 1) return FILE_UNAVAILABLE;
    OpenFile* open = lookup(owner, handle);
    if (!open) return FILE_BAD_HANDLE;
    if (!(open->flags & FILE_WRITE)) return FILE_ACCESS;
    if (length && !data) return FILE_BAD;
    StoredFile* file = &stored_files[open->file];
    if (open->offset > file->size) return FILE_BAD;
    if (length > FILE_CAPACITY - open->offset) return FILE_NO_SPACE;
    if (!length) return 0;
    memcpy(file->data + open->offset, data, length);
    uint32_t end = open->offset + length;
    if (end > file->size) file->size = end;
    int result = commit();
    if (result) return result;
    open->offset = end;
    return (int)length;
}

int files_seek(uint64_t owner, uint64_t handle, uint32_t offset) {
    if (!services_owned() || volume.state != 1) return FILE_UNAVAILABLE;
    OpenFile* open = lookup(owner, handle);
    if (!open) return FILE_BAD_HANDLE;
    if (offset > stored_files[open->file].size) return FILE_BAD;
    open->offset = offset;
    return 0;
}

int files_close(uint64_t owner, uint64_t handle) {
    if (!services_owned()) return FILE_UNAVAILABLE;
    OpenFile* open = lookup(owner, handle);
    if (!open) return FILE_BAD_HANDLE;
    open->owner = 0;
    return 0;
}

void files_release(uint64_t owner) {
    if (!services_owned() || !owner) return;
    for (unsigned int i = 0; i < FILE_HANDLE_LIMIT; i++)
        if (open_files[i].owner == owner) open_files[i].owner = 0;
}

int files_list(uint64_t owner, FileEntry* entry) {
    if (!services_owned() || !owner || volume.state != 1) return FILE_UNAVAILABLE;
    if (!entry || entry->cursor > FILE_LIMIT) return FILE_BAD;
    unsigned int start = entry->cursor;
    *entry = (FileEntry){0};
    for (unsigned int i = start; i < FILE_LIMIT; i++) {
        if (!stored_files[i].used) continue;
        entry->cursor = i + 1;
        entry->size = stored_files[i].size;
        memcpy(entry->path, prefix, 5);
        for (unsigned int j = 0; j < NAME_MAX; j++) entry->path[j + 5] = stored_files[i].name[j];
        return 1;
    }
    entry->cursor = FILE_LIMIT;
    return 0;
}

int files_remove(uint64_t owner, const char* path) {
    char name[NAME_MAX] = {0};
    if (!services_owned() || !owner || volume.state != 1) return FILE_UNAVAILABLE;
    if (!name_from_path(path, name)) return FILE_BAD;
    for (unsigned int i = 0; i < FILE_LIMIT; i++) {
        if (!stored_files[i].used || !same_name(stored_files[i].name, name)) continue;
        for (unsigned int j = 0; j < FILE_HANDLE_LIMIT; j++)
            if (open_files[j].owner && open_files[j].file == i) return FILE_BUSY;
        stored_files[i] = (StoredFile){0};
        return commit();
    }
    return FILE_NOT_FOUND;
}
