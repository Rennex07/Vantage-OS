#pragma once
#include <vantage/base.h>

/* Shared syscall payload. Allocation APIs remain kernel-only. */
typedef struct {
    uint64_t ram_bytes;
    uint64_t allocatable_pages;
    uint64_t free_pages;
    uint64_t heap_bytes;
    uint64_t heap_used_bytes;
} MemoryStats;
