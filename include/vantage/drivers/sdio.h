#pragma once
#include <vantage/drivers/hardware.h>

typedef struct { const HwOps* io; uint32_t source_hz, rca; int state, busy, error; } Sdio;
/* Pi 4 legacy EMMC at FE300000, GPIO 34..39. Never accesses EMMC2/SD pins. */
int sdio_init(Sdio* host, const HwOps* io);
int sdio_direct(Sdio* host, unsigned int function, uint32_t address, int write, uint8_t* value);
int sdio_transfer(Sdio* host, unsigned int function, uint32_t address, int increment,
                   int write, void* data, unsigned int bytes);
