#include "../../includes/files_internal.h"
#include "../../includes/platform.h"

static int valid(uintptr_t address, size_t size, unsigned int alignment) {
    return !(address % alignment) && user_buffer_valid(address, size);
}

int64_t handle_file_call(uint64_t id, uintptr_t argument, uint64_t owner) {
    if (!owner) return FILE_UNAVAILABLE;
    if (id == SYS_FILE_OPEN) {
        if (!valid(argument, sizeof(FileOpen), 4)) return FILE_BAD;
        const FileOpen request = *(const FileOpen*)argument;
        if (request.reserved) return FILE_BAD;
        return files_open(owner, request.path, request.flags);
    }
    if (id == SYS_FILE_READ || id == SYS_FILE_WRITE) {
        if (!valid(argument, sizeof(FileIo), 8)) return FILE_BAD;
        FileIo request = *(const FileIo*)argument;
        if (request.reserved || request.length > FILE_IO_MAX) return FILE_BAD;
        if (id == SYS_FILE_WRITE)
            return files_write(owner, request.handle, request.data, request.length);
        memset(request.data, 0, sizeof(request.data));
        int result = files_read(owner, request.handle, request.data, request.length);
        if (result >= 0) {
            request.length = (uint32_t)result;
            *(FileIo*)argument = request;
        }
        return result;
    }
    if (id == SYS_FILE_SEEK) {
        if (!valid(argument, sizeof(FileSeek), 8)) return FILE_BAD;
        const FileSeek request = *(const FileSeek*)argument;
        if (request.reserved) return FILE_BAD;
        return files_seek(owner, request.handle, request.offset);
    }
    if (id == SYS_FILE_CLOSE) return files_close(owner, argument);
    if (id == SYS_FILE_LIST) {
        if (!valid(argument, sizeof(FileEntry), 4)) return FILE_BAD;
        FileEntry entry = *(const FileEntry*)argument;
        int result = files_list(owner, &entry);
        if (result >= 0) *(FileEntry*)argument = entry;
        return result;
    }
    if (id == SYS_FILE_REMOVE) {
        if (!valid(argument, sizeof(FilePath), 1)) return FILE_BAD;
        const FilePath path = *(const FilePath*)argument;
        return files_remove(owner, path.path);
    }
    return FILE_BAD;
}
