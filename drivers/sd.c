/* SD command/state engine. Register and protocol references: docs/SD.md. */
#include "../includes/sd.h"

enum {
    BLOCK = 0x04, ARGUMENT = 0x08, COMMAND = 0x0c, RESPONSE = 0x10,
    DATA = 0x20, STATE = 0x24, HOST = 0x28, CLOCK = 0x2c,
    INTERRUPT = 0x30, STATUS_ENABLE = 0x34, SIGNAL_ENABLE = 0x38,
    VERSION = 0xfc
};

#define BIT(n) (1U << (n))
#define COMMAND_BUSY BIT(0)
#define DATA_BUSY BIT(1)
#define CLOCK_OUTPUT BIT(2)
#define RESET_ALL BIT(24)
#define IRQ_COMMAND BIT(0)
#define IRQ_TRANSFER BIT(1)
#define IRQ_READ BIT(5)
#define IRQ_WRITE BIT(4)
#define IRQ_ERRORS 0xffff8000U
#define IRQ_TIMEOUTS (BIT(16) | BIT(20))
#define R1_ERRORS 0xfff98008U
#define COMMAND_TIMEOUT_US 100000U
#define CLOCK_TIMEOUT_US 150000U
#define DATA_TIMEOUT_US 1000000U
#define WRITE_TIMEOUT_US 5000000U
#define ERASE_TIMEOUT_US 120000000U
#define POLL_LIMIT 100000U

enum { RESPONSE_NONE, RESPONSE_LONG, RESPONSE_SHORT, RESPONSE_BUSY };

static uint32_t read_reg(SdCard* card, uint32_t offset) {
    return card->io.read32(card->io.context, offset);
}

static void write_reg(SdCard* card, uint32_t offset, uint32_t value) {
    card->io.write32(card->io.context, offset, value);
}

static uint64_t now(SdCard* card) { return card->io.now_us(card->io.context); }

static int wait_bits(SdCard* card, uint32_t offset, uint32_t mask,
                     uint32_t expected, uint32_t timeout) {
    uint64_t start = now(card);
    uint32_t attempts;
    for (attempts = 0; attempts < POLL_LIMIT; attempts++) {
        if ((read_reg(card, offset) & mask) == expected) return SD_OK;
        if (now(card) - start >= timeout) break;
        card->io.delay_us(card->io.context, timeout / POLL_LIMIT > 10 ? timeout / POLL_LIMIT : 10);
    }
    return SD_TIMEOUT;
}

static int wait_interrupt(SdCard* card, uint32_t wanted, uint32_t timeout) {
    uint64_t start = now(card);
    uint32_t attempts;
    for (attempts = 0; attempts < POLL_LIMIT; attempts++) {
        uint32_t status = read_reg(card, INTERRUPT);
        card->last_interrupt = status;
        /* An error takes precedence even if the completion bit is also set. */
        if (status & IRQ_ERRORS) return status & IRQ_TIMEOUTS ? SD_TIMEOUT : SD_ERROR;
        if (status & wanted) {
            write_reg(card, INTERRUPT, wanted); /* Write-one-to-clear, not RMW. */
            return SD_OK;
        }
        if (now(card) - start >= timeout) break;
        card->io.delay_us(card->io.context, timeout / POLL_LIMIT > 10 ? timeout / POLL_LIMIT : 10);
    }
    return SD_TIMEOUT;
}

static int invalidate(SdCard* card, int result) {
    card->ready = 0;
    write_reg(card, SIGNAL_ENABLE, 0);
    /* Abort through reset, not by gating a clock while CMD/DAT is inhibited.
     * If a wedged host cannot reset, keep the primary error and never reuse it. */
    write_reg(card, CLOCK, read_reg(card, CLOCK) | RESET_ALL);
    (void)wait_bits(card, CLOCK, RESET_ALL, 0, COMMAND_TIMEOUT_US);
    return result;
}

static int set_clock(SdCard* card, uint32_t target) {
    uint32_t divisor, value;
    int result = wait_bits(card, STATE, COMMAND_BUSY | DATA_BUSY, 0, COMMAND_TIMEOUT_US);
    if (result) return result;
    /* SDHCI v3 divided-clock mode: register N produces base / (2*N).
     * Round up, so the wire clock never exceeds the card's requested limit. */
    divisor = (uint32_t)(((uint64_t)card->base_clock_hz + 2U * target - 1) / (2U * target));
    if (!divisor || divisor > 1023) return SD_UNSUPPORTED;
    write_reg(card, CLOCK, read_reg(card, CLOCK) & ~CLOCK_OUTPUT);
    value = (14U << 16) | BIT(0) | ((divisor & 255U) << 8) | ((divisor >> 8) << 6);
    write_reg(card, CLOCK, value);
    result = wait_bits(card, CLOCK, BIT(1), BIT(1), CLOCK_TIMEOUT_US);
    if (result) return result;
    write_reg(card, CLOCK, value | CLOCK_OUTPUT);
    card->clock_hz = card->base_clock_hz / (2U * divisor);
    return SD_OK;
}

static int command(SdCard* card, uint32_t index, uint32_t argument,
                   uint32_t response_type, uint32_t check_status, uint32_t data) {
    uint32_t flags = response_type << 16;
    uint32_t inhibit = COMMAND_BUSY;
    int result;
    if (data || response_type == RESPONSE_BUSY) inhibit |= DATA_BUSY;
    result = wait_bits(card, STATE, inhibit, 0, COMMAND_TIMEOUT_US);
    if (result) return result;
    if (response_type == RESPONSE_LONG) flags |= BIT(19); /* CRC, no index in R2. */
    else if (response_type != RESPONSE_NONE && index != 41) flags |= BIT(19) | BIT(20);
    if (data) flags |= BIT(21) | (data == 1 ? BIT(4) : 0); /* Single block, PIO. */
    write_reg(card, INTERRUPT, 0xffffffffU);
    write_reg(card, ARGUMENT, argument);
    write_reg(card, COMMAND, (index << 24) | flags);
    result = wait_interrupt(card, IRQ_COMMAND, COMMAND_TIMEOUT_US);
    if (result) return result;
    card->last_response = response_type == RESPONSE_NONE ? 0 : read_reg(card, RESPONSE);
    if (check_status && (card->last_response & R1_ERRORS)) return SD_ERROR;
    if (response_type == RESPONSE_BUSY) {
        uint32_t timeout = index == 38 ? ERASE_TIMEOUT_US : DATA_TIMEOUT_US;
        result = wait_interrupt(card, IRQ_TRANSFER, timeout);
        if (!result) result = wait_bits(card, STATE, DATA_BUSY, 0, timeout);
    }
    return result;
}

/* SDHCI stores R2 card bits [127:8] in response-register bits [119:0]. */
static uint32_t csd_field(const uint32_t response[4], uint32_t low, uint32_t width) {
    uint32_t i, value = 0;
    for (i = 0; i < width; i++) {
        uint32_t position = low + i - 8;
        value |= ((response[position / 32] >> (position % 32)) & 1U) << i;
    }
    return value;
}

static int capacity(SdCard* card) {
    uint32_t response[4], structure, block_length, i;
    for (i = 0; i < 4; i++) response[i] = read_reg(card, RESPONSE + i * 4);
    structure = csd_field(response, 126, 2);
    block_length = csd_field(response, 80, 4);
    card->command_classes = csd_field(response, 84, 12);
    card->write_protected = csd_field(response, 12, 2) != 0;
    uint32_t write_length = csd_field(response, 22, 4);
    if (write_length >= 9 && write_length <= 11) {
        card->erase_sectors = csd_field(response, 46, 1) ? 1U :
            (csd_field(response, 39, 7) + 1U) << (write_length - 9);
    }
    if (card->block_addressed) {
        if (structure != 1 || block_length != 9) return SD_UNSUPPORTED;
        card->sectors = ((uint64_t)csd_field(response, 48, 22) + 1) * 1024;
    } else {
        uint32_t multiplier = csd_field(response, 47, 3);
        if (structure != 0 || block_length < 9 || block_length > 11) return SD_UNSUPPORTED;
        card->sectors = ((uint64_t)csd_field(response, 62, 12) + 1) << (multiplier + 2 + block_length - 9);
        if (card->sectors > 0x400000ULL) return SD_UNSUPPORTED;
    }
    return SD_OK;
}

int sd_card_init(SdCard* card, const SdIo* io, uint32_t base_clock_hz) {
    uint64_t start;
    uint32_t attempts, ocr = 0;
    int result;
    if (!card || !io || !io->read32 || !io->write32 || !io->now_us || !io->delay_us ||
        base_clock_hz < 400000 || base_clock_hz > 818400000) return SD_INVALID;
    *card = (SdCard){0};
    card->io = *io;
    card->base_clock_hz = base_clock_hz;
    /* This engine intentionally requires v3's 10-bit divider, with v4 mode off. */
    if (((read_reg(card, VERSION) >> 16) & 255U) < 2) return SD_UNSUPPORTED;
    write_reg(card, SIGNAL_ENABLE, 0);
    write_reg(card, CLOCK, RESET_ALL);
    result = wait_bits(card, CLOCK, RESET_ALL, 0, COMMAND_TIMEOUT_US);
    if (result) return invalidate(card, result);
    write_reg(card, HOST, read_reg(card, HOST) & ~(BIT(1) | BIT(2) | BIT(5)));
    write_reg(card, STATUS_ENABLE, IRQ_COMMAND | IRQ_TRANSFER | IRQ_READ | IRQ_WRITE | IRQ_ERRORS);
    result = set_clock(card, 400000);
    if (result) return invalidate(card, result);
    card->io.delay_us(card->io.context, 1000); /* More than 74 initial SD clocks. */
    result = command(card, 0, 0, RESPONSE_NONE, 0, 0);
    if (!result) result = command(card, 8, 0x1aa, RESPONSE_SHORT, 0, 0);
    if (!result && card->last_response != 0x1aa) result = SD_UNSUPPORTED;
    if (result) return invalidate(card, result);
    start = now(card);
    for (attempts = 0; attempts < 1000; attempts++) {
        result = command(card, 55, 0, RESPONSE_SHORT, 1, 0);
        if (result) return invalidate(card, result);
        if (!(card->last_response & BIT(5))) return invalidate(card, SD_UNSUPPORTED);
        /* Request 2.7-3.6V and high capacity, not the 1.8V signaling switch. */
        result = command(card, 41, BIT(30) | 0x00ff8000U, RESPONSE_SHORT, 0, 0);
        if (result) return invalidate(card, result);
        ocr = card->last_response;
        if (ocr & BIT(31)) break;
        if (now(card) - start >= DATA_TIMEOUT_US) return invalidate(card, SD_TIMEOUT);
        card->io.delay_us(card->io.context, 1000);
    }
    if (!(ocr & BIT(31))) return invalidate(card, SD_TIMEOUT);
    if (!(ocr & 0x00ff8000U)) return invalidate(card, SD_UNSUPPORTED);
    card->block_addressed = (ocr >> 30) & 1U;
    result = command(card, 2, 0, RESPONSE_LONG, 0, 0);
    if (!result) result = command(card, 3, 0, RESPONSE_SHORT, 0, 0);
    if (result) return invalidate(card, result);
    if (card->last_response & 0xe000U) return invalidate(card, SD_ERROR); /* R6 errors. */
    card->rca = card->last_response & 0xffff0000U;
    if (!card->rca) return invalidate(card, SD_ERROR);
    result = command(card, 9, card->rca, RESPONSE_LONG, 0, 0);
    if (!result) result = capacity(card);
    if (!result) result = command(card, 7, card->rca, RESPONSE_BUSY, 1, 0);
    if (!result && !card->block_addressed)
        result = command(card, 16, SD_BLOCK_SIZE, RESPONSE_SHORT, 1, 0);
    if (!result) result = set_clock(card, 25000000);
    if (result) return invalidate(card, result);
    card->ready = 1;
    return SD_OK;
}

int sd_card_sync(SdCard* card) {
    if (!card) return SD_INVALID;
    if (!card->ready) return SD_NOT_READY;
    uint64_t start = now(card);
    for (uint32_t attempts = 0; attempts < 5000; attempts++) {
        int result = command(card, 13, card->rca, RESPONSE_SHORT, 1, 0);
        if (result) return invalidate(card, result);
        uint32_t state = (card->last_response >> 9) & 15U;
        if ((card->last_response & BIT(8)) && state == 4) {
            result = wait_bits(card, STATE, DATA_BUSY, 0, WRITE_TIMEOUT_US);
            return result ? invalidate(card, result) : SD_OK;
        }
        if (state != 4 && state != 7) return invalidate(card, SD_ERROR);
        if (now(card) - start >= WRITE_TIMEOUT_US) break;
        card->io.delay_us(card->io.context, 1000);
    }
    return invalidate(card, SD_TIMEOUT);
}

int sd_card_write(SdCard* card, uint64_t lba, uint32_t count,
                  const void* buffer, size_t buffer_bytes) {
    if (!card) return SD_INVALID;
    if (!card->ready) return SD_NOT_READY;
    if (!buffer || !count || count > SD_READ_MAX_BLOCKS) return SD_INVALID;
    size_t needed = (size_t)count * SD_BLOCK_SIZE;
    if (buffer_bytes < needed || (uintptr_t)buffer > ~(uintptr_t)0 - needed ||
        lba >= card->sectors || count > card->sectors - lba) return SD_INVALID;
    if (card->write_protected) return SD_WRITE_PROTECTED;
    if (!(card->command_classes & BIT(4))) return SD_UNSUPPORTED;
    const uint8_t* bytes = buffer;
    for (uint32_t block = 0; block < count; block++) {
        uint64_t address = card->block_addressed ? lba + block : (lba + block) * SD_BLOCK_SIZE;
        if (address > 0xffffffffULL) return invalidate(card, SD_ERROR);
        write_reg(card, BLOCK, (1U << 16) | SD_BLOCK_SIZE);
        int result = command(card, 24, (uint32_t)address, RESPONSE_SHORT, 1, 2);
        if (!result) result = wait_interrupt(card, IRQ_WRITE, DATA_TIMEOUT_US);
        if (result) return invalidate(card, result);
        for (uint32_t word = 0; word < SD_BLOCK_SIZE / 4; word++, bytes += 4) {
            uint32_t value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                             ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
            write_reg(card, DATA, value);
        }
        result = wait_interrupt(card, IRQ_TRANSFER, WRITE_TIMEOUT_US);
        if (!result) result = wait_bits(card, STATE, DATA_BUSY, 0, WRITE_TIMEOUT_US);
        if (!result) result = sd_card_sync(card);
        if (result) return card->ready ? invalidate(card, result) : result;
    }
    return SD_OK;
}

int sd_card_erase(SdCard* card, uint64_t lba, uint32_t count) {
    if (!card) return SD_INVALID;
    if (!card->ready) return SD_NOT_READY;
    if (!count || count > SD_ERASE_MAX_BLOCKS || lba >= card->sectors ||
        count > card->sectors - lba) return SD_INVALID;
    if (card->write_protected) return SD_WRITE_PROTECTED;
    if (!(card->command_classes & BIT(5)) || !card->erase_sectors) return SD_UNSUPPORTED;
    if (lba % card->erase_sectors || count % card->erase_sectors) return SD_INVALID;
    uint64_t first = card->block_addressed ? lba : lba * SD_BLOCK_SIZE;
    uint64_t last = card->block_addressed ? lba + count - 1 : (lba + count - 1) * SD_BLOCK_SIZE;
    if (last > 0xffffffffULL) return SD_INVALID;
    int result = command(card, 32, (uint32_t)first, RESPONSE_SHORT, 1, 0);
    if (!result) result = command(card, 33, (uint32_t)last, RESPONSE_SHORT, 1, 0);
    if (!result) result = command(card, 38, 0, RESPONSE_BUSY, 1, 0);
    if (!result) result = sd_card_sync(card);
    return result && card->ready ? invalidate(card, result) : result;
}

int sd_card_read(SdCard* card, uint64_t lba, uint32_t count,
                 void* buffer, size_t buffer_bytes) {
    uint8_t* bytes = buffer;
    uint32_t block, word;
    size_t needed;
    if (!card) return SD_INVALID;
    if (!card->ready) return SD_NOT_READY;
    if (!buffer || !count || count > SD_READ_MAX_BLOCKS) return SD_INVALID;
    needed = (size_t)count * SD_BLOCK_SIZE;
    if (buffer_bytes < needed || (uintptr_t)buffer > ~(uintptr_t)0 - needed ||
        lba >= card->sectors || count > card->sectors - lba) return SD_INVALID;
    for (block = 0; block < count; block++) {
        uint64_t address = card->block_addressed ? lba + block : (lba + block) * SD_BLOCK_SIZE;
        int result;
        if (address > 0xffffffffULL) return invalidate(card, SD_ERROR);
        write_reg(card, BLOCK, (1U << 16) | SD_BLOCK_SIZE);
        result = command(card, 17, (uint32_t)address, RESPONSE_SHORT, 1, 1);
        if (!result) result = wait_interrupt(card, IRQ_READ, DATA_TIMEOUT_US);
        if (result) return invalidate(card, result);
        for (word = 0; word < SD_BLOCK_SIZE / 4; word++) {
            uint32_t value = read_reg(card, DATA);
            /* The controller's FIFO is word-sized; destinations need not be. */
            *bytes++ = (uint8_t)value;
            *bytes++ = (uint8_t)(value >> 8);
            *bytes++ = (uint8_t)(value >> 16);
            *bytes++ = (uint8_t)(value >> 24);
        }
        result = wait_interrupt(card, IRQ_TRANSFER, DATA_TIMEOUT_US);
        if (!result) result = wait_bits(card, STATE, DATA_BUSY, 0, DATA_TIMEOUT_US);
        if (result) return invalidate(card, result);
    }
    return SD_OK;
}
