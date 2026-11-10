#pragma once
#include <vantage/drivers/sd.h>

/* Pi 4 EMMC2 adapter: boot/core-0-pinned tasks, EL1, masked exceptions,
 * exclusive ownership. Movable tasks are rejected even while on core 0.
 * The input clock must be obtained from firmware/board configuration and remain
 * fixed. Normal boot deliberately does not call this unvalidated hardware path. */
int sd_init(uint32_t base_clock_hz);
int sd_read_blocks(uint64_t lba, uint32_t count, void* buffer, size_t buffer_bytes);
int sd_write_blocks(uint64_t lba, uint32_t count, const void* buffer, size_t buffer_bytes);
int sd_sync(void);
int sd_erase_blocks(uint64_t lba, uint32_t count);
uint64_t sd_sector_count(void);
/* Legacy byte-count API: caller supplies room for count * 512 bytes. */
int sd_readBlock(unsigned int lba, unsigned char* buffer, unsigned int count);
