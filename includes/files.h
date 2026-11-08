#pragma once
#include "std.h"

#define FILE_PATH_MAX 48U
#define FILE_IO_MAX 256U
#define FILE_LIMIT 16U
#define FILE_HANDLE_LIMIT 16U
#define FILE_CAPACITY 4096U

#define FILE_READ 1U
#define FILE_WRITE 2U
#define FILE_CREATE 4U
#define FILE_TRUNCATE 8U

enum FileError {
    FILE_BAD = -1, FILE_NOT_FOUND = -2, FILE_NO_SPACE = -3,
    FILE_ACCESS = -4, FILE_BAD_HANDLE = -5, FILE_BUSY = -6,
    FILE_UNAVAILABLE = -7, FILE_CORRUPT = -8, FILE_IO_ERROR = -9
};

#define SYS_FILE_OPEN 32U
#define SYS_FILE_READ 33U
#define SYS_FILE_WRITE 34U
#define SYS_FILE_SEEK 35U
#define SYS_FILE_CLOSE 36U
#define SYS_FILE_LIST 37U
#define SYS_FILE_REMOVE 38U

typedef struct {
    uint32_t flags, reserved;
    char path[FILE_PATH_MAX];
} FileOpen;

typedef struct {
    uint64_t handle;
    uint32_t length, reserved;
    unsigned char data[FILE_IO_MAX];
} FileIo;

typedef struct {
    uint64_t handle;
    uint32_t offset, reserved;
} FileSeek;

typedef struct { char path[FILE_PATH_MAX]; } FilePath;
typedef struct {
    uint32_t cursor, size;
    char path[FILE_PATH_MAX];
} FileEntry;
