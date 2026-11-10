#include <vantage/drivers/hardware.h>

#define POLL_LIMIT 8000000U

int hw_ops_valid(const HwOps* io) {
    return io && io->read && io->write && io->time_us && io->owned;
}

int hw_wait(const HwOps* io, uintptr_t address, uint32_t mask, uint32_t value,
            uint32_t timeout_us) {
    if (!hw_ops_valid(io) || !timeout_us || (value & ~mask)) return HW_INVALID;
    uint32_t start = io->time_us(io->context);
    for (unsigned int i = 0; i < POLL_LIMIT; i++) {
        if ((io->read(io->context, address) & mask) == value) return HW_OK;
        if (io->time_us(io->context) - start >= timeout_us) return HW_TIMEOUT;
    }
    return HW_TIMEOUT;
}

int hw_delay(const HwOps* io, uint32_t delay_us) {
    if (!hw_ops_valid(io) || delay_us > 1000000U) return HW_INVALID;
    uint32_t start = io->time_us(io->context);
    for (unsigned int i = 0; i < POLL_LIMIT; i++)
        if (io->time_us(io->context) - start >= delay_us) return HW_OK;
    return HW_TIMEOUT;
}
