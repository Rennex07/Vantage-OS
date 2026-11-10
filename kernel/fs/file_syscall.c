#include <vantage/kernel/files.h>
#include <vantage/kernel/user_access.h>

/* Requests are snapshotted from the calling process and results copied back;
 * the file layer only ever sees kernel memory. */
static int fetch(void* request, uintptr_t address, size_t size, unsigned int alignment) {
    return !(address % alignment) && user_copy_in(request, address, size);
}

int64_t handle_file_call(uint64_t id, uintptr_t argument, uint64_t owner) {
    if (!owner) return FILE_UNAVAILABLE;
    if (id == SYS_FILE_OPEN) {
        FileOpen request;
        if (!fetch(&request, argument, sizeof(request), 4)) return FILE_BAD;
        if (request.reserved) return FILE_BAD;
        return files_open(owner, request.path, request.flags);
    }
    if (id == SYS_FILE_READ || id == SYS_FILE_WRITE) {
        FileIo request;
        if (!fetch(&request, argument, sizeof(request), 8)) return FILE_BAD;
        /* The file position must not move for a result that cannot be stored. */
        if (id == SYS_FILE_READ && !user_access_ok(argument, sizeof(request), 1)) return FILE_BAD;
        if (request.reserved || request.length > FILE_IO_MAX) return FILE_BAD;
        if (id == SYS_FILE_WRITE)
            return files_write(owner, request.handle, request.data, request.length);
        memset(request.data, 0, sizeof(request.data));
        int result = files_read(owner, request.handle, request.data, request.length);
        if (result >= 0) {
            request.length = (uint32_t)result;
            if (!user_copy_out(argument, &request, sizeof(request))) return FILE_BAD;
        }
        return result;
    }
    if (id == SYS_FILE_SEEK) {
        FileSeek request;
        if (!fetch(&request, argument, sizeof(request), 8)) return FILE_BAD;
        if (request.reserved) return FILE_BAD;
        return files_seek(owner, request.handle, request.offset);
    }
    if (id == SYS_FILE_CLOSE) return files_close(owner, argument);
    if (id == SYS_FILE_LIST) {
        FileEntry entry;
        if (!fetch(&entry, argument, sizeof(entry), 4) ||
            !user_access_ok(argument, sizeof(entry), 1)) return FILE_BAD;
        int result = files_list(owner, &entry);
        if (result >= 0 && !user_copy_out(argument, &entry, sizeof(entry))) return FILE_BAD;
        return result;
    }
    if (id == SYS_FILE_REMOVE) {
        FilePath path;
        if (!fetch(&path, argument, sizeof(path), 1)) return FILE_BAD;
        return files_remove(owner, path.path);
    }
    return FILE_BAD;
}
