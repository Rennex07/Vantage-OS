#pragma once
#include <vantage/base.h>

enum HwResult {
    HW_OK = 1, HW_INVALID = 0, HW_TIMEOUT = -1, HW_PROTOCOL = -2,
    HW_UNSUPPORTED = -3, HW_BUSY = -4, HW_IO = -5, HW_OFF = -6,
    HW_NO_SPACE = -7
};
enum HwCache { HW_CLEAN, HW_INVALIDATE, HW_CLEAN_INVALIDATE };

typedef struct HwOps HwOps;
struct HwOps {
    void* context;
    uint32_t (*read)(void*, uintptr_t);
    void (*write)(void*, uintptr_t, uint32_t);
    uint32_t (*time_us)(void*);
    int (*owned)(void*);
    void (*cache)(void*, void*, size_t, enum HwCache);
    uint64_t (*physical)(void*, const void*);
    int (*property)(void*, uint32_t, uint32_t*, unsigned int);
};

int hw_ops_valid(const HwOps* io);
int hw_wait(const HwOps* io, uintptr_t address, uint32_t mask, uint32_t value,
            uint32_t timeout_us);
int hw_delay(const HwOps* io, uint32_t delay_us);

#ifdef _MSC_VER
#define HW_ALIGN(n) __declspec(align(n))
#else
#define HW_ALIGN(n) __attribute__((aligned(n)))
#endif

/* A timed-out property buffer can still be firmware-owned. Retain this object
 * for kernel lifetime; timeout quarantines it until reboot. */
typedef struct {
    HW_ALIGN(64) uint32_t words[32];
    int state;
} PropertyMailbox;
int property_call(PropertyMailbox* mailbox, const HwOps* io, uint32_t tag,
                  uint32_t* values, unsigned int count);
