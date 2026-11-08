#include "../../includes/wifi.h"

static int direct(void* context, unsigned int function, uint32_t address, int write, uint8_t* value) {
    return sdio_direct(context, function, address, write, value);
}
static int transfer(void* context, unsigned int function, uint32_t address, int increment,
                      int write, void* data, unsigned int bytes) {
    return sdio_transfer(context, function, address, increment, write, data, bytes);
}
static uint32_t time_us(void* context) {
    Sdio* host = context;
    return host->io->time_us(host->io->context);
}
static int delay(void* context, uint32_t us) {
    Sdio* host = context;
    return hw_delay(host->io, us);
}
static int owned(void* context) {
    Sdio* host = context;
    return host && host->state == 1 && host->io->owned(host->io->context);
}
WifiBus wifi_sdio_bus(Sdio* host) {
    WifiBus bus = {host, direct, transfer, time_us, delay, owned};
    return bus;
}
