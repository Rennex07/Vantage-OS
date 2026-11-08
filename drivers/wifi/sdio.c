/* SDHCI/SDIO register definitions: BCM2711 ARM Peripherals and SDIO CCCR.
 * The removable SD driver owns EMMC2; this host is dedicated to onboard WLAN. */
#include "../../includes/sdio.h"

#define BASE 0xfe300000ULL
#define GPIO 0xfe200000ULL
#define ERROR_MASK 0xffff8000U

static uint32_t rd(Sdio* h, unsigned int offset) { return h->io->read(h->io->context, BASE + offset); }
static void wr(Sdio* h, unsigned int offset, uint32_t value) { h->io->write(h->io->context, BASE + offset, value); }
static int stop(Sdio* h, int error) {
    h->error = error; h->state = -1;
    wr(h, 0x38, 0);
    wr(h, 0x2c, rd(h, 0x2c) | (3U << 25));
    (void)hw_wait(h->io, BASE + 0x2c, 3U << 25, 0, 100000);
    return error;
}

static int interrupt(Sdio* h, uint32_t wanted) {
    uint32_t start = h->io->time_us(h->io->context);
    for (unsigned int i = 0; i < 8000000U; i++) {
        uint32_t status = rd(h, 0x30);
        if (status & ERROR_MASK) return stop(h, HW_IO);
        if (status & wanted) { wr(h, 0x30, wanted); return HW_OK; }
        if (h->io->time_us(h->io->context) - start >= 500000U) break;
    }
    return stop(h, HW_TIMEOUT);
}

static int command(Sdio* h, unsigned int index, uint32_t argument, uint32_t flags, uint32_t* response) {
    uint32_t inhibit = 1U | ((flags & ((1U << 21) | (1U << 16))) ? 2U : 0U);
    if (hw_wait(h->io, BASE + 0x24, inhibit, 0, 500000) != HW_OK) return stop(h, HW_TIMEOUT);
    wr(h, 0x30, 0xffff8033U); /* Command/data errors and transfer status, not card IRQ. */
    wr(h, 0x08, argument);
    wr(h, 0x0c, (index << 24) | flags);
    int result = interrupt(h, 1);
    if (result != HW_OK) return result;
    uint32_t value = rd(h, 0x10);
    if (index == 52 || index == 53) {
        if (value & 0xcb00U) return stop(h, HW_PROTOCOL);
    } else if (index == 3 && (value & 0xe000U)) return stop(h, HW_PROTOCOL);
    else if (index == 7 && (value & 0xfdffe008U)) return stop(h, HW_PROTOCOL);
    if (response) *response = value;
    if ((flags & (3U << 16)) == (3U << 16)) return interrupt(h, 2);
    return HW_OK;
}

static int direct(Sdio* h, unsigned int function, uint32_t address, int write, uint8_t* value) {
    uint32_t response = 0;
    int result = command(h, 52, (write ? 1U << 31 : 0) | (function << 28) |
                         (address << 9) | (write ? *value : 0), (2U << 16) | (3U << 19), &response);
    if (result == HW_OK && !write) *value = (uint8_t)response;
    return result;
}

static int clock_set(Sdio* h, uint32_t hz) {
    if (!hz) return HW_INVALID;
    uint32_t divisor = h->source_hz <= hz ? 0 : (uint32_t)(((uint64_t)h->source_hz + 2ULL * hz - 1) / (2ULL * hz));
    if (divisor > 1023) return HW_INVALID;
    wr(h, 0x2c, rd(h, 0x2c) & ~4U);
    uint32_t value = (14U << 16) | ((divisor & 255U) << 8) | ((divisor >> 8) << 6) | 1U;
    wr(h, 0x2c, value);
    if (hw_wait(h->io, BASE + 0x2c, 2, 2, 500000) != HW_OK) return stop(h, HW_TIMEOUT);
    wr(h, 0x2c, value | 4U);
    return hw_delay(h->io, 2000);
}

int sdio_init(Sdio* h, const HwOps* io) {
    if (!h || !hw_ops_valid(io) || !io->property || !io->owned(io->context)) return HW_INVALID;
    if (h->state) return HW_BUSY;
    h->io = io; h->state = 2;
    uint32_t clock[2] = {1, 0};
    if (io->property(io->context, 0x00030002, clock, 2) != HW_OK || clock[0] != 1 || !clock[1])
        return h->state = -1, HW_IO;
    h->source_hz = clock[1];
    /* Firmware GPIO expander 1 = WL_ON. No pin-header/SD-slot GPIO changes. */
    for (unsigned int power = 0; power < 2; power++) {
        uint32_t gpio[2] = {129, power};
        if (io->property(io->context, 0x00038041, gpio, 2) != HW_OK || gpio[0] != 129)
            return h->state = -1, HW_IO;
        if (hw_delay(io, power ? 150000 : 10000) != HW_OK) return h->state = -1, HW_TIMEOUT;
    }
    for (unsigned int pin = 34; pin <= 39; pin++) {
        uintptr_t select = GPIO + (pin / 10) * 4;
        uint32_t shift = (pin % 10) * 3;
        uint32_t value = io->read(io->context, select);
        io->write(io->context, select, (value & ~(7U << shift)) | (7U << shift)); /* ALT3 */
        uintptr_t pull = GPIO + 0xe4 + (pin / 16) * 4;
        shift = (pin % 16) * 2;
        value = io->read(io->context, pull);
        io->write(io->context, pull, (value & ~(3U << shift)) | ((pin == 34 ? 0U : 1U) << shift));
    }
    wr(h, 0x38, 0); wr(h, 0x2c, 1U << 24);
    if (hw_wait(io, BASE + 0x2c, 7U << 24, 0, 500000) != HW_OK) return stop(h, HW_TIMEOUT);
    if (((rd(h, 0xfc) >> 16) & 255U) < 2) return h->state = -1, HW_UNSUPPORTED;
    wr(h, 0x28, 0); wr(h, 0x34, ~0U); wr(h, 0x38, 0);
    if (clock_set(h, 400000) != HW_OK) return h->state = -1, HW_TIMEOUT;
    if (command(h, 0, 0, 0, NULL) != HW_OK) return h->error;
    uint32_t ocr = 0;
    if (command(h, 5, 0, 2U << 16, &ocr) != HW_OK) return h->error;
    uint32_t voltage = ocr & (3U << 20);
    if (!voltage || ((ocr >> 28) & 7U) < 2) return h->state = -1, HW_UNSUPPORTED;
    uint32_t start = io->time_us(io->context);
    for (unsigned int attempt = 0; !(ocr & 0x80000000U); attempt++) {
        if (attempt == 100 || io->time_us(io->context) - start >= 1000000U) return stop(h, HW_TIMEOUT);
        if (command(h, 5, voltage, 2U << 16, &ocr) != HW_OK || hw_delay(io, 10000) != HW_OK)
            return stop(h, HW_IO);
    }
    uint32_t response;
    if (command(h, 3, 0, (2U << 16) | (3U << 19), &response) != HW_OK) return h->error;
    h->rca = response & 0xffff0000U;
    if (!h->rca) return stop(h, HW_PROTOCOL);
    if (command(h, 7, h->rca, (3U << 16) | (3U << 19), NULL) != HW_OK) return h->error;
    uint8_t value = 2;
    if (direct(h, 0, 7, 1, &value) != HW_OK) return h->error;
    wr(h, 0x28, rd(h, 0x28) | 2U);
    for (unsigned int function = 1; function <= 2; function++) {
        unsigned int bytes = function == 1 ? 64U : 512U;
        value = (uint8_t)bytes;
        if (direct(h, 0, function * 256 + 0x10, 1, &value) != HW_OK) return h->error;
        value = (uint8_t)(bytes >> 8);
        if (direct(h, 0, function * 256 + 0x11, 1, &value) != HW_OK) return h->error;
    }
    value = 2;
    if (direct(h, 0, 2, 1, &value) != HW_OK) return h->error;
    start = io->time_us(io->context);
    for (unsigned int tries = 0;; tries++) {
        if (direct(h, 0, 3, 0, &value) != HW_OK) return h->error;
        if (value & 2) break;
        if (tries == 1000 || io->time_us(io->context) - start >= 500000U) return stop(h, HW_TIMEOUT);
    }
    value = 0;
    if (direct(h, 0, 4, 1, &value) != HW_OK || clock_set(h, 25000000) != HW_OK) return stop(h, HW_IO);
    h->state = 1; h->error = 0;
    return HW_OK;
}

int sdio_direct(Sdio* h, unsigned int function, uint32_t address, int write, uint8_t* value) {
    if (!h || !h->io || !value || function > 7 || address > 0x1ffffU ||
        (write != 0 && write != 1) || !h->io->owned(h->io->context)) return HW_INVALID;
    if (h->state != 1) return HW_OFF;
    if (h->busy) return HW_BUSY;
    h->busy = 1;
    int result = direct(h, function, address, write, value);
    h->busy = 0;
    return result;
}

int sdio_transfer(Sdio* h, unsigned int function, uint32_t address, int increment,
                   int write, void* data, unsigned int bytes) {
    if (!h || !h->io || !data || !bytes || bytes > 512 || function > 7 || address > 0x1ffffU ||
        (increment && bytes > 0x20000U - address) || (write != 0 && write != 1) ||
        (increment != 0 && increment != 1) || !h->io->owned(h->io->context)) return HW_INVALID;
    if (h->state != 1) return HW_OFF;
    if (h->busy) return HW_BUSY;
    h->busy = 1;
    wr(h, 0x04, (1U << 16) | bytes);
    uint32_t argument = (write ? 1U << 31 : 0) | (function << 28) |
                        (increment ? 1U << 26 : 0) | (address << 9) | (bytes & 511U);
    uint32_t flags = (2U << 16) | (3U << 19) | (1U << 21) | (write ? 0U : 16U);
    int result = command(h, 53, argument, flags, NULL);
    if (result == HW_OK) result = interrupt(h, write ? 16U : 32U);
    if (result == HW_OK) {
        unsigned char* buffer = data;
        for (unsigned int offset = 0; offset < bytes; offset += 4) {
            uint32_t value = 0;
            if (!write) value = rd(h, 0x20);
            for (unsigned int j = 0; j < 4 && offset + j < bytes; j++) {
                if (write) value |= (uint32_t)buffer[offset + j] << (8 * j);
                else buffer[offset + j] = (unsigned char)(value >> (8 * j));
            }
            if (write) wr(h, 0x20, value);
        }
        result = interrupt(h, 2);
    }
    h->busy = 0;
    return result;
}
