#include "../../includes/sd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

/* An independent SDHCI model with W1C status and a FIFO that produces transfer
 * completion only after all 128 words have been read. No driver internals are
 * included: tests exercise its public API, compiled as a separate source. */
typedef struct {
    uint32_t reg[64], trace[8192], arguments[8192], commands;
    uint64_t time;
    uint32_t accesses, writes, words, lba, read_commands, initial_clock;
    uint32_t echo, ocr, rca, csd[4], app_response;
    uint32_t acmd_count, ready_after;
    uint32_t fail_command, fail_irq, silent_command, bad_status_command;
    uint32_t no_read_ready, no_transfer, stuck_data, stuck_reset, unstable_clock, frozen_time;
    uint32_t data_error, fail_read_number;
    uint32_t write_commands, erase_first, erase_last, no_write_ready, status_busy, status_error;
    uint32_t written;
    uint8_t media[8][512];
} Host;

static Host host;
static SdCard card;

static uint8_t disk_byte(uint32_t sector, uint32_t offset) {
    return (uint8_t)(sector * 131U + offset);
}

static uint32_t host_read(void* context, uint32_t offset) {
    Host* h = context;
    CHECK(h == &host && !(offset & 3) && offset < 256);
    h->accesses++;
    if (offset == 0x20) {
        uint32_t result = 0, i;
        CHECK(h->words < 128 && (h->reg[0x24 / 4] & 2));
        for (i = 0; i < 4; i++) {
            uint8_t byte = h->lba < 8 && (h->written & (1U << h->lba)) ?
                h->media[h->lba][h->words * 4 + i] : disk_byte(h->lba, h->words * 4 + i);
            result |= (uint32_t)byte << (i * 8);
        }
        if (++h->words == 128) {
            if (!h->no_transfer) h->reg[0x30 / 4] |= 2;
            if (h->data_error) h->reg[0x30 / 4] |= 0x00208000;
            if (!h->stuck_data) h->reg[0x24 / 4] &= ~2U;
        }
        return result;
    }
    return h->reg[offset / 4];
}

static void host_write(void* context, uint32_t offset, uint32_t value) {
    Host* h = context;
    uint32_t index, argument, i, expected_flags;
    CHECK(h == &host && !(offset & 3) && offset < 256);
    h->accesses++; h->writes++;
    if (offset == 0x20) {
        CHECK(h->lba < 8 && h->words < 128 && (h->reg[0x24 / 4] & 2));
        for (i = 0; i < 4; i++) h->media[h->lba][h->words * 4 + i] = (uint8_t)(value >> (8 * i));
        h->written |= 1U << h->lba;
        if (++h->words == 128) {
            if (!h->no_transfer) h->reg[0x30 / 4] |= 2;
            if (h->data_error) h->reg[0x30 / 4] |= 0x00208000;
            if (!h->stuck_data) h->reg[0x24 / 4] &= ~2U;
        }
        return;
    }
    if (offset == 0x30) { h->reg[offset / 4] &= ~value; return; }
    if (offset == 0x2c) {
        if (!(value & 0x01000000) && (h->reg[offset / 4] & 4) && !(value & 4))
            CHECK(!(h->reg[0x24 / 4] & 3)); /* No clock gating during a transaction. */
        if (value & 0x01000000) {
            h->reg[offset / 4] = h->stuck_reset ? value : 0;
            if (!h->stuck_reset) h->reg[0x24 / 4] = h->reg[0x30 / 4] = 0;
        }
        else h->reg[offset / 4] = value | ((value & 1) && !h->unstable_clock ? 2U : 0U);
        return;
    }
    h->reg[offset / 4] = value;
    if (offset != 0x0c) return;
    CHECK(h->reg[0x2c / 4] & 4); /* No command without the wire clock. */
    CHECK(h->reg[0x38 / 4] == 0); /* Polling must never enable CPU IRQ signaling. */
    index = value >> 24;
    argument = h->reg[0x08 / 4];
    CHECK(h->commands < 8192);
    h->trace[h->commands] = index;
    h->arguments[h->commands++] = argument;
    expected_flags = index == 0 ? 0 : index == 2 || index == 9 ? 0x00090000U :
                     index == 41 ? 0x00020000U : index == 7 ? 0x001b0000U :
                     index == 17 ? 0x003a0010U : index == 24 ? 0x003a0000U :
                     index == 38 ? 0x001b0000U : 0x001a0000U;
    CHECK((value & 0xffffff) == expected_flags);
    if (index == h->silent_command) return;
    h->reg[0x30 / 4] |= 1;
    if (index == h->fail_command) { h->reg[0x30 / 4] |= h->fail_irq; return; }
    h->reg[0x10 / 4] = 0;
    switch (index) {
        case 0: CHECK(argument == 0); h->initial_clock = h->reg[0x2c / 4]; break;
        case 8: CHECK(argument == 0x1aa); h->reg[0x10 / 4] = h->echo; break;
        case 55: CHECK(argument == 0); h->reg[0x10 / 4] = h->app_response; break;
        case 41:
            CHECK(h->commands >= 2 && h->trace[h->commands - 2] == 55);
            CHECK(argument == 0x40ff8000); /* HCS, no 1.8V request. */
            h->reg[0x10 / 4] = h->ocr;
            if (++h->acmd_count < h->ready_after) h->reg[0x10 / 4] &= ~0x80000000U;
            break;
        case 2: CHECK(argument == 0); break;
        case 3: CHECK(argument == 0); h->reg[0x10 / 4] = h->rca; break;
        case 9:
            CHECK(argument == (h->rca & 0xffff0000U));
            for (i = 0; i < 4; i++) h->reg[0x10 / 4 + i] = h->csd[i];
            break;
        case 7:
            CHECK(argument == (h->rca & 0xffff0000U));
            if (!h->no_transfer) h->reg[0x30 / 4] |= 2;
            if (h->stuck_data) h->reg[0x24 / 4] |= 2;
            break;
        case 16: CHECK(argument == 512 && !(h->ocr & 0x40000000)); break;
        case 13:
            CHECK(argument == (h->rca & 0xffff0000U));
            h->reg[0x10 / 4] = h->status_error ? 0x04000900U : h->status_busy ? 0xe00U : 0x900U;
            break;
        case 24:
            CHECK(h->reg[0x04 / 4] == 0x00010200);
            h->lba = h->ocr & 0x40000000U ? argument : argument / 512;
            CHECK(h->lba < 8);
            if (!(h->ocr & 0x40000000U)) CHECK(!(argument % 512));
            h->words = 0; h->write_commands++; h->reg[0x24 / 4] |= 2;
            if (!h->no_write_ready) h->reg[0x30 / 4] |= 16;
            break;
        case 32: h->erase_first = argument; break;
        case 33: h->erase_last = argument; break;
        case 38:
            CHECK(argument == 0 && h->commands >= 3 && h->trace[h->commands - 3] == 32 &&
                  h->trace[h->commands - 2] == 33);
            for (i = 0; i < 8; i++) {
                uint32_t address = h->ocr & 0x40000000U ? i : i * 512;
                if (address >= h->erase_first && address <= h->erase_last) {
                    memset(h->media[i], 0xff, 512); h->written |= 1U << i;
                }
            }
            if (!h->no_transfer) h->reg[0x30 / 4] |= 2;
            break;
        case 17:
            CHECK(h->reg[0x04 / 4] == 0x00010200);
            h->lba = h->ocr & 0x40000000 ? argument : argument / 512;
            if (!(h->ocr & 0x40000000)) CHECK(!(argument % 512));
            h->read_commands++; h->words = 0;
            if (h->read_commands == h->fail_read_number) h->reg[0x30 / 4] |= 0x00208000;
            h->reg[0x24 / 4] |= 2;
            if (!h->no_read_ready) h->reg[0x30 / 4] |= 0x20;
            break;
        default: CHECK(0);
    }
    if (index == h->bad_status_command) h->reg[0x10 / 4] |= 0x00400000;
}

static uint64_t host_now(void* context) { return ((Host*)context)->time; }
static void host_delay(void* context, uint32_t duration) {
    Host* h = context;
    if (!h->frozen_time) h->time += duration;
}

static SdIo io = {&host, host_read, host_write, host_now, host_delay};

static void reset(void) {
    memset(&host, 0, sizeof(host)); memset(&card, 0, sizeof(card));
    host.reg[0xfc / 4] = 0x00020000;
    host.echo = 0x1aa;
    host.ocr = 0xc0ff8000;
    host.rca = 0x12340000;
    host.app_response = 0x20;
    host.ready_after = 2;
    host.csd[3] = 0x00400000; /* CSD_STRUCTURE=1, READ_BL_LEN=9, C_SIZE=8191. */
    host.csd[2] = 0x00000900;
    host.csd[1] = 0x001fff00;
    host.csd[2] |= 0x00030000U; /* Write and erase command classes. */
    host.csd[1] |= 0x40; /* ERASE_BLK_EN. */
    host.csd[0] |= 9U << 14; /* WRITE_BL_LEN. */
    host.fail_command = host.silent_command = host.bad_status_command = ~0U;
}

static void init_ok(void) {
    CHECK(sd_card_init(&card, &io, 100000000) == SD_OK && card.ready);
    CHECK(card.sectors == 8388608 && card.block_addressed && card.rca == 0x12340000);
    CHECK(card.clock_hz == 25000000);
    CHECK((host.initial_clock & 0xffc0U) == 0x7d00U);
    CHECK(host.commands == 10);
    { const uint32_t expected[] = {0, 8, 55, 41, 55, 41, 2, 3, 9, 7};
      CHECK(!memcmp(expected, host.trace, sizeof(expected))); }
    CHECK(host.reg[0x30 / 4] == 0);
}

static void failed_init(int expected) {
    CHECK(sd_card_init(&card, &io, 100000000) == expected);
    CHECK(!card.ready && !(host.reg[0x2c / 4] & 4));
    CHECK(sd_card_read(&card, 0, 1, host.trace, 512) == SD_NOT_READY);
}

static void test_initialization(void) {
    uint32_t i;
    reset(); init_ok();
    for (i = 0; i < 4; i++) {
        const uint32_t bad[] = {0, 0xaa, 0x1ab, 0xdeadbeef};
        reset(); host.echo = bad[i]; failed_init(SD_UNSUPPORTED); CHECK(host.commands == 2);
    }
    reset(); host.app_response = 0; failed_init(SD_UNSUPPORTED);
    reset(); host.ocr = 0xc0000000; failed_init(SD_UNSUPPORTED);
    reset(); host.ocr = 0x40ff8000; failed_init(SD_TIMEOUT);
    reset(); host.rca = 0; failed_init(SD_ERROR);
    reset(); host.rca |= 0x2000; failed_init(SD_ERROR);
    reset(); host.csd[3] = 0x00800000; failed_init(SD_UNSUPPORTED);
    reset(); host.csd[2] = 0x800; failed_init(SD_UNSUPPORTED);
    reset(); host.csd[3] = 0; failed_init(SD_UNSUPPORTED); /* OCR/CSD disagree. */
    reset(); host.ocr &= ~0x40000000U; failed_init(SD_UNSUPPORTED);
    reset(); host.ocr &= ~0x40000000U; host.csd[3] = 0;
    host.csd[2] = 0xb03; host.csd[1] = 0xffffc380;
    failed_init(SD_UNSUPPORTED); /* CSD v1 claims more than SDSC's 2 GiB limit. */
    reset(); host.bad_status_command = 55; failed_init(SD_ERROR);
    reset(); host.bad_status_command = 7; failed_init(SD_ERROR);
    reset(); host.stuck_reset = 1; failed_init(SD_TIMEOUT);
    reset(); host.unstable_clock = 1; failed_init(SD_TIMEOUT);
    reset(); host.no_transfer = 1; failed_init(SD_TIMEOUT);
    reset(); host.stuck_data = 1; failed_init(SD_TIMEOUT);
    reset(); host.reg[0xfc / 4] = 0x10000; failed_init(SD_UNSUPPORTED); CHECK(!host.writes);
    reset(); host.silent_command = 8; failed_init(SD_TIMEOUT);
    reset(); host.frozen_time = 1; host.silent_command = 8; failed_init(SD_TIMEOUT);
    CHECK(host.accesses < 100100); /* Iteration bound even if clock callback stalls. */
    for (i = 16; i <= 23; i++) {
        reset(); host.fail_command = 8; host.fail_irq = 1U << i;
        failed_init(i == 16 || i == 20 ? SD_TIMEOUT : SD_ERROR);
        CHECK(card.last_interrupt & (1U << i));
    }
    reset(); host.time = ~(uint64_t)0 - 500; init_ok(); /* Wrapping microsecond counter. */
}

static void test_reads(void) {
    uint8_t data[2 * 512 + 2];
    static uint8_t large[SD_READ_MAX_BLOCKS * SD_BLOCK_SIZE + 2];
    uint32_t i, accesses;
    reset(); init_ok(); memset(data, 0xa7, sizeof(data));
    CHECK(sd_card_read(&card, 7, 2, data + 1, 1024) == SD_OK);
    CHECK(data[0] == 0xa7 && data[1025] == 0xa7);
    CHECK(host.read_commands == 2 && host.words == 128);
    for (i = 0; i < 1024; i++) CHECK(data[i + 1] == disk_byte(7 + i / 512, i % 512));
    CHECK(host.arguments[host.commands - 2] == 7 && host.arguments[host.commands - 1] == 8);
    accesses = host.accesses;
    CHECK(sd_card_read(NULL, 0, 1, data, 512) == SD_INVALID);
    CHECK(sd_card_read(&card, 0, 1, NULL, 512) == SD_INVALID);
    CHECK(sd_card_read(&card, 0, 0, data, 512) == SD_INVALID);
    CHECK(sd_card_read(&card, 0, 129, data, 512) == SD_INVALID);
    CHECK(sd_card_read(&card, 0, 1, data, 511) == SD_INVALID);
    CHECK(sd_card_read(&card, 0, 1, (void*)(uintptr_t)(~(uintptr_t)0 - 10), 512) == SD_INVALID);
    CHECK(sd_card_read(&card, card.sectors, 1, data, 512) == SD_INVALID);
    CHECK(sd_card_read(&card, card.sectors - 1, 2, data, 1024) == SD_INVALID);
    CHECK(sd_card_read(&card, ~(uint64_t)0, 1, data, 512) == SD_INVALID);
    CHECK(host.accesses == accesses && card.ready);
    CHECK(sd_card_read(&card, card.sectors - 1, 1, data, 512) == SD_OK);
    reset(); host.csd[1] = 0x3fffff00;
    CHECK(sd_card_init(&card, &io, 100000000) == SD_OK && card.sectors == 0x100000000ULL);
    CHECK(sd_card_read(&card, 0xffffffffULL, 1, data, 512) == SD_OK);
    CHECK(host.arguments[host.commands - 1] == 0xffffffffU);
    reset(); host.ocr &= ~0x40000000U;
    host.csd[3] = 0; host.csd[2] = 0x900; host.csd[1] = 0xffc00380;
    CHECK(sd_card_init(&card, &io, 100000000) == SD_OK && card.sectors == 524288);
    CHECK(!card.block_addressed && host.trace[host.commands - 1] == 16);
    CHECK(sd_card_read(&card, 17, 1, data, 512) == SD_OK);
    CHECK(host.arguments[host.commands - 1] == 17 * 512);
    for (i = 0; i < 512; i++) CHECK(data[i] == disk_byte(17, i));
    reset(); init_ok(); memset(large, 0xa7, sizeof(large));
    CHECK(sd_card_read(&card, 100, SD_READ_MAX_BLOCKS, large + 1, sizeof(large) - 2) == SD_OK);
    CHECK(large[0] == 0xa7 && large[sizeof(large) - 1] == 0xa7);
    for (i = 0; i < sizeof(large) - 2; i++) CHECK(large[i + 1] == disk_byte(100 + i / 512, i % 512));
    reset(); init_ok(); memset(data, 0xa7, sizeof(data)); host.fail_read_number = 2;
    CHECK(sd_card_read(&card, 7, 2, data + 1, 1024) == SD_ERROR && !card.ready);
    for (i = 0; i < 512; i++) CHECK(data[i + 1] == disk_byte(7, i));
    for (i = 513; i < sizeof(data); i++) CHECK(data[i] == 0xa7);
}

static void test_failed_reads(void) {
    uint8_t data[512];
    uint32_t scenario;
    for (scenario = 0; scenario < 10; scenario++) {
        uint32_t accesses;
        int result;
        reset(); init_ok();
        if (scenario == 0) host.no_read_ready = 1;
        if (scenario == 1) host.no_transfer = 1;
        if (scenario == 2) host.stuck_data = 1;
        if (scenario == 3) host.bad_status_command = 17;
        if (scenario == 4) { host.fail_command = 17; host.fail_irq = 0x00208000; }
        if (scenario == 5) host.reg[0x24 / 4] = 1;
        if (scenario == 6) host.reg[0x24 / 4] = 2;
        if (scenario == 7) { host.fail_command = 17; host.fail_irq = 0x00108000; }
        if (scenario == 8) host.data_error = 1;
        if (scenario == 9) {
            host.fail_command = 17; host.fail_irq = 0x00208000; host.stuck_reset = 1;
        }
        result = sd_card_read(&card, 0, 1, data, sizeof(data));
        CHECK(result == (scenario == 3 || scenario == 4 || scenario >= 8 ? SD_ERROR : SD_TIMEOUT));
        CHECK(!card.ready);
        if (scenario != 9) CHECK(!(host.reg[0x2c / 4] & 4));
        else CHECK(host.reg[0x2c / 4] & 0x01000000); /* Failed abort remains unready. */
        accesses = host.accesses;
        CHECK(sd_card_read(&card, 0, 1, data, sizeof(data)) == SD_NOT_READY);
        CHECK(accesses == host.accesses);
        host.no_read_ready = host.no_transfer = host.stuck_data = 0;
        host.data_error = host.stuck_reset = 0;
        host.bad_status_command = host.fail_command = ~0U;
        host.reg[0x24 / 4] = 0;
        CHECK(sd_card_init(&card, &io, 100000000) == SD_OK);
        CHECK(sd_card_read(&card, 0, 1, data, sizeof(data)) == SD_OK);
    }
}

static void test_configuration(void) {
    uint32_t accesses, rate;
    SdIo invalid;
    reset(); accesses = host.accesses;
    CHECK(sd_card_init(NULL, &io, 100000000) == SD_INVALID);
    CHECK(sd_card_init(&card, NULL, 100000000) == SD_INVALID);
    invalid = io; invalid.read32 = NULL;
    CHECK(sd_card_init(&card, &invalid, 100000000) == SD_INVALID);
    invalid = io; invalid.write32 = NULL;
    CHECK(sd_card_init(&card, &invalid, 100000000) == SD_INVALID);
    invalid = io; invalid.now_us = NULL;
    CHECK(sd_card_init(&card, &invalid, 100000000) == SD_INVALID);
    invalid = io; invalid.delay_us = NULL;
    CHECK(sd_card_init(&card, &invalid, 100000000) == SD_INVALID);
    CHECK(sd_card_init(&card, &io, 0) == SD_INVALID);
    CHECK(sd_card_init(&card, &io, 399999) == SD_INVALID);
    CHECK(sd_card_init(&card, &io, 818400001) == SD_INVALID);
    CHECK(accesses == host.accesses);
    for (rate = 41000000; rate <= 500000000; rate += 41000000) {
        uint32_t encoded, divider;
        reset(); CHECK(sd_card_init(&card, &io, rate) == SD_OK);
        encoded = host.reg[0x2c / 4];
        divider = ((encoded >> 8) & 255U) | ((encoded & 0xc0U) << 2);
        CHECK(divider && card.clock_hz == rate / (divider * 2));
        CHECK(card.clock_hz <= 25000000);
    }
    reset(); CHECK(sd_card_init(&card, &io, 500000000) == SD_OK);
    CHECK((host.initial_clock & 0xffc0U) == 0x7180U); /* N=625, both upper bits matter. */
    reset(); CHECK(sd_card_init(&card, &io, 818400000) == SD_OK);
    CHECK((host.initial_clock & 0xffc0U) == 0xffc0U); /* N=1023. */
}

static void test_writes_and_erase(void) {
    uint8_t data[1026], readback[1024];
    for (unsigned int i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 17);
    reset(); init_ok();
    CHECK(card.erase_sectors == 1);
    CHECK(sd_card_write(&card, 2, 2, data + 1, 1024) == SD_OK && host.write_commands == 2);
    CHECK(sd_card_read(&card, 2, 2, readback, sizeof(readback)) == SD_OK);
    CHECK(!memcmp(readback, data + 1, 1024));
    CHECK(sd_card_erase(&card, 2, 2) == SD_OK && host.erase_first == 2 && host.erase_last == 3);
    CHECK(sd_card_read(&card, 2, 2, readback, sizeof(readback)) == SD_OK);
    for (unsigned int i = 0; i < sizeof(readback); i++) CHECK(readback[i] == 0xff);
    CHECK(host.written == ((1U << 2) | (1U << 3)));
    uint32_t accesses = host.accesses;
    CHECK(sd_card_write(&card, 0, 1, NULL, 512) == SD_INVALID);
    CHECK(sd_card_write(&card, 0, 1, data, 511) == SD_INVALID);
    CHECK(sd_card_write(&card, 0, 0, data, 512) == SD_INVALID);
    CHECK(sd_card_write(&card, 0, 1, (void*)(uintptr_t)(~(uintptr_t)0 - 10), 512) == SD_INVALID);
    CHECK(sd_card_write(&card, card.sectors, 1, data, 512) == SD_INVALID);
    CHECK(sd_card_write(&card, 0, 129, data, sizeof(data)) == SD_INVALID);
    CHECK(sd_card_erase(&card, card.sectors - 1, 2) == SD_INVALID);
    CHECK(sd_card_erase(&card, 0, 0) == SD_INVALID);
    card.erase_sectors = 4;
    CHECK(sd_card_erase(&card, 1, 4) == SD_INVALID);
    CHECK(sd_card_erase(&card, 0, 2) == SD_INVALID);
    card.write_protected = 1;
    CHECK(sd_card_write(&card, 0, 1, data, 512) == SD_WRITE_PROTECTED);
    CHECK(sd_card_erase(&card, 0, 4) == SD_WRITE_PROTECTED);
    CHECK(host.accesses == accesses);
    card.write_protected = 0; card.command_classes = 0;
    CHECK(sd_card_write(&card, 0, 1, data, 512) == SD_UNSUPPORTED);
    CHECK(sd_card_erase(&card, 0, 4) == SD_UNSUPPORTED && host.accesses == accesses);
    for (unsigned int scenario = 0; scenario < 8; scenario++) {
        reset(); init_ok();
        if (scenario == 0) host.no_write_ready = 1;
        if (scenario == 1) host.no_transfer = 1;
        if (scenario == 2) host.stuck_data = 1;
        if (scenario == 3) host.data_error = 1;
        if (scenario == 4) host.bad_status_command = 24;
        if (scenario == 5) host.status_error = 1;
        if (scenario == 6) host.status_busy = 1;
        if (scenario == 7) { host.fail_command = 24; host.fail_irq = 0x10000; }
        CHECK(sd_card_write(&card, 0, 1, data, 512) != SD_OK && !card.ready);
        CHECK(sd_card_write(&card, 0, 1, data, 512) == SD_NOT_READY);
    }
    for (unsigned int index = 32; index <= 38; index++) {
        if (index != 32 && index != 33 && index != 38) continue;
        reset(); init_ok(); host.bad_status_command = index;
        CHECK(sd_card_erase(&card, 0, 1) == SD_ERROR && !card.ready);
    }
    reset(); host.ocr &= ~0x40000000U;
    host.csd[3] = 0; host.csd[2] = 0x30900; host.csd[1] = 0xffc003c0;
    CHECK(sd_card_init(&card, &io, 100000000) == SD_OK);
    CHECK(sd_card_write(&card, 2, 1, data + 1, 512) == SD_OK);
    CHECK(host.arguments[host.commands - 2] == 1024);
    CHECK(sd_card_erase(&card, 2, 2) == SD_OK && host.erase_first == 1024 && host.erase_last == 1536);
    reset(); host.csd[0] |= 1U << 4; init_ok(); CHECK(card.write_protected);
    reset(); host.csd[1] = 0x3fffff40;
    CHECK(sd_card_init(&card, &io, 100000000) == SD_OK);
    CHECK(sd_card_erase(&card, 0xffffffffULL, 1) == SD_OK && host.erase_last == 0xffffffffU);
    reset(); host.ocr &= ~0x40000000U; host.csd[3] = 0; host.csd[2] = 0x30900;
    host.csd[0] = (10U << 14) | (1U << 31); host.csd[1] = 0xffc00387;
    CHECK(sd_card_init(&card, &io, 100000000) == SD_OK && card.erase_sectors == 32);
    accesses = host.accesses;
    CHECK(sd_card_erase(&card, 1, 32) == SD_INVALID && host.accesses == accesses);
    reset(); init_ok(); host.no_transfer = 1;
    CHECK(sd_card_erase(&card, 0, 1) == SD_TIMEOUT && !card.ready);
}

int main(void) {
    test_configuration(); test_initialization(); test_reads(); test_failed_reads(); test_writes_and_erase();
    puts("PASS: SD initialization, capacity, byte/block addressing, PIO read/write, erase, status and failure recovery");
    return 0;
}
