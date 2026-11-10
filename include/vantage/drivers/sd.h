#pragma once
#include <vantage/base.h>

#define SD_BLOCK_SIZE 512U
#define SD_READ_MAX_BLOCKS 128U
#define SD_ERASE_MAX_BLOCKS 65536U

enum {
    SD_OK = 0,
    SD_TIMEOUT = -1,
    SD_ERROR = -2,
    SD_INVALID = -3,
    SD_UNSUPPORTED = -4,
    SD_NOT_READY = -5,
    SD_WRITE_PROTECTED = -6
};

/* Offsets are relative to the host controller. Callbacks must return promptly.
 * now_us is a monotonic wrapping 64-bit microsecond counter; delay_us waits at
 * least the requested interval. The caller exclusively owns the controller. */
typedef struct {
    void* context;
    uint32_t (*read32)(void* context, uint32_t offset);
    void (*write32)(void* context, uint32_t offset, uint32_t value);
    uint64_t (*now_us)(void* context);
    void (*delay_us)(void* context, uint32_t microseconds);
} SdIo;

typedef struct {
    SdIo io;
    uint64_t sectors;
    uint32_t base_clock_hz, clock_hz;
    uint32_t rca, block_addressed, ready;
    uint32_t last_interrupt, last_response;
    uint32_t erase_sectors, write_protected, command_classes;
} SdCard;

/* SD v2 SDSC/SDHC/SDXC, 1-bit bus, default speed, programmed I/O.
 * Use a zero-initialized or previously initialized card and a separate SdIo.
 * Firmware/board code must establish power, 3.3V signaling and clock routing.
 * Any hardware/protocol failure invalidates the card until reinitialized. */
int sd_card_init(SdCard* card, const SdIo* io, uint32_t base_clock_hz);
/* Returns SD_OK only after all sectors complete. On error the destination can
 * contain partial data and must be discarded. Invalid arguments cause no I/O. */
int sd_card_read(SdCard* card, uint64_t lba, uint32_t count,
                  void* buffer, size_t buffer_bytes);
/* Single-sector CMD24 transactions. A failed request may have changed sectors;
 * never retry an uncertain filesystem commit without remount/recovery. */
int sd_card_write(SdCard* card, uint64_t lba, uint32_t count,
                  const void* buffer, size_t buffer_bytes);
/* CMD13 readiness/status check; does not enable an optional card write cache. */
int sd_card_sync(SdCard* card);
/* Destructive, inclusive CMD32/33 range followed by normal CMD38 (not discard).
 * Range must align to CSD erase units. Erased byte values are unspecified. */
int sd_card_erase(SdCard* card, uint64_t lba, uint32_t count);
