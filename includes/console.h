#pragma once
#include "memory.h"
#include "input/key.h"
#include "video/text.h"

enum { CONSOLE_MEMORY_UNAVAILABLE, CONSOLE_MEMORY_KERNEL, CONSOLE_MEMORY_HOST };

typedef struct {
    uint32_t columns;
    uint32_t rows;
    uint32_t memory_kind;
    uint32_t reserved;
    MemoryStats memory;
    uint64_t host_backbuffer_bytes;
} ConsoleInfo;

#define CONSOLE_WRITE_MAX 128U
typedef struct {
    uint32_t length;
    char text[CONSOLE_WRITE_MAX];
} ConsoleWrite;

/* Returns host display allocation bytes, not emulated kernel/RAM statistics. */
#ifdef VANTAGE_SIM
uint64_t sim_backbuffer_bytes(void);
#endif
