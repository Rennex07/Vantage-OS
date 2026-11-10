/* CYW43455 SDIO/backplane and SDPCM/BCDC protocol, referenced against
 * Linux v6.12 brcmfmac (ISC, Copyright 2010/2014 Broadcom Corporation).
 * See docs/licenses/brcmfmac-ISC.txt and docs/HARDWARE.md. */
#include <vantage/drivers/wifi.h>

#define RAM_BASE 0x198000U
#define ENUM_BASE 0x18000000U
#define CLOCK_CSR 0x1000eU

static uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static unsigned int le16(const unsigned char* p) { return (unsigned int)p[0] | ((unsigned int)p[1] << 8); }
static uint32_t be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put16(unsigned char* p, unsigned int value) { p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8); }
static void put32(unsigned char* p, uint32_t value) {
    for (unsigned int i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (i * 8));
}
static void erase(void* memory, size_t bytes) {
    volatile unsigned char* p = memory;
    while (bytes--) *p++ = 0;
}
static int fail(Wifi* w, int result) {
    w->state = -1; w->error = result; w->link = w->keyed = w->connected = 0;
    erase(w->tx, sizeof(w->tx)); erase(w->control, sizeof(w->control));
    erase(w->rx, sizeof(w->rx)); erase(w->nvram, sizeof(w->nvram));
    return result;
}
static int reg8(Wifi* w, unsigned int fn, uint32_t address, int write, uint8_t* value) {
    return w->bus.direct(w->bus.context, fn, address, write, value);
}
static int set8(Wifi* w, unsigned int fn, uint32_t address, uint8_t value) { return reg8(w, fn, address, 1, &value); }
static int wait8(Wifi* w, unsigned int fn, uint32_t address, uint8_t mask, uint8_t expected, uint32_t us) {
    uint32_t start = w->bus.time_us(w->bus.context);
    for (unsigned int i = 0; i < 100000; i++) {
        uint8_t value;
        int result = reg8(w, fn, address, 0, &value);
        if (result != HW_OK) return result;
        if ((value & mask) == expected) return HW_OK;
        if (w->bus.time_us(w->bus.context) - start >= us) break;
    }
    return HW_TIMEOUT;
}

static int window(Wifi* w, uint32_t address) {
    address &= ~0x7fffU;
    if (w->window == address) return HW_OK;
    for (unsigned int i = 0; i < 3; i++) {
        int result = set8(w, 1, 0x1000a + i, (uint8_t)(address >> (8 * (i + 1))));
        if (result != HW_OK) return result;
    }
    w->window = address;
    return HW_OK;
}

static int backplane(Wifi* w, uint32_t address, void* data, unsigned int bytes, int write) {
    if (!bytes || address > 0xffffffffU - bytes) return HW_INVALID;
    unsigned char* p = data;
    while (bytes) {
        unsigned int count = 0x8000U - (address & 0x7fffU);
        if (count > bytes) count = bytes;
        if (count > 512) count = 512;
        int result = window(w, address);
        if (result != HW_OK) return result;
        if (count >= 4) {
            count &= ~3U;
            result = w->bus.transfer(w->bus.context, 1, (address & 0x7fffU) | 0x8000U, 1, write, p, count);
        } else {
            for (unsigned int i = 0; i < count && result == HW_OK; i++)
                result = reg8(w, 1, ((address + i) & 0x7fffU) | 0x8000U, write, p + i);
        }
        if (result != HW_OK) return result;
        address += count; p += count; bytes -= count;
    }
    return HW_OK;
}
static int read32(Wifi* w, uint32_t address, uint32_t* value) {
    unsigned char data[4];
    int result = backplane(w, address, data, 4, 0);
    if (result == HW_OK) *value = le32(data);
    return result;
}
static int write32(Wifi* w, uint32_t address, uint32_t value) {
    unsigned char data[4]; put32(data, value);
    return backplane(w, address, data, 4, 1);
}
static int wait32(Wifi* w, uint32_t address, uint32_t mask, uint32_t expected) {
    uint32_t start = w->bus.time_us(w->bus.context);
    for (unsigned int i = 0; i < 10000; i++) {
        uint32_t value;
        int result = read32(w, address, &value);
        if (result != HW_OK) return result;
        if ((value & mask) == expected) return HW_OK;
        if (w->bus.time_us(w->bus.context) - start >= 500000U) break;
    }
    return HW_TIMEOUT;
}

static int core_disable(Wifi* w, uint32_t wrap, uint32_t before, uint32_t after) {
    uint32_t reset;
    if (read32(w, wrap + 0x800, &reset) != HW_OK) return HW_IO;
    if (!(reset & 1)) {
        if (write32(w, wrap + 0x408, 3U | before) != HW_OK ||
            read32(w, wrap + 0x408, &reset) != HW_OK ||
            write32(w, wrap + 0x800, 1) != HW_OK ||
            wait32(w, wrap + 0x800, 1, 1) != HW_OK) return HW_IO;
    }
    return write32(w, wrap + 0x408, 3U | after);
}
static int core_reset(Wifi* w, uint32_t wrap, uint32_t before, uint32_t after) {
    int result = core_disable(w, wrap, before, after);
    if (result != HW_OK) return result;
    if (write32(w, wrap + 0x800, 0) != HW_OK || wait32(w, wrap + 0x800, 1, 0) != HW_OK ||
        w->bus.delay(w->bus.context, 40) != HW_OK) return HW_IO;
    return write32(w, wrap + 0x408, 1U | after);
}

static int chip_probe(Wifi* w) {
    uint32_t value, erom;
    if (read32(w, ENUM_BASE, &value) != HW_OK) return HW_IO;
    w->chip = value & 0xffffU; w->revision = (value >> 16) & 15U;
    if (w->chip != 0x4345 || w->revision != 6 || (value >> 28) != 1) return HW_UNSUPPORTED;
    if (read32(w, ENUM_BASE + 0xfc, &erom) != HW_OK || (erom & 3) ||
        erom < ENUM_BASE || erom > 0x18200000U - sizeof(w->erom)) return HW_PROTOCOL;
    if (backplane(w, erom, w->erom, sizeof(w->erom), 0) != HW_OK) return HW_IO;
    unsigned int core = 0, core_revision = 0, ended = 0;
    for (unsigned int offset = 0; offset + 4 <= sizeof(w->erom); offset += 4) {
        value = le32(w->erom + offset);
        unsigned int type = value & 15U;
        if (type == 15) { ended = 1; break; }
        if (!(value & 1)) continue;
        if (type == 1) {
            if (offset + 8 > sizeof(w->erom) || (le32(w->erom + offset + 4) & 15U) != 1) return HW_PROTOCOL;
            core = (value >> 8) & 0xfffU;
            core_revision = le32(w->erom + offset + 4) >> 24;
            offset += 4;
        } else if ((type & ~8U) == 5) {
            if (type & 8) {
                if (offset + 8 > sizeof(w->erom) || le32(w->erom + offset + 4)) return HW_UNSUPPORTED;
                offset += 4;
            }
            unsigned int size_type = (value >> 4) & 3U;
            if (size_type == 3) {
                if (offset + 8 > sizeof(w->erom)) return HW_PROTOCOL;
                offset += 4;
                if (le32(w->erom + offset) & 8U) {
                    if (offset + 8 > sizeof(w->erom)) return HW_PROTOCOL;
                    offset += 4;
                }
            }
            if (size_type > 1) continue;
            uint32_t address = value & 0xfffff000U;
            if (address < ENUM_BASE || address >= 0x18200000U) return HW_PROTOCOL;
            unsigned int kind = (value >> 6) & 3U;
            if (core == 0x83e && kind == 0 && !w->arm) w->arm = address;
            if (core == 0x83e && kind >= 2 && !w->arm_wrap) w->arm_wrap = address;
            if (core == 0x812 && kind >= 2 && !w->d11_wrap) w->d11_wrap = address;
            if (core == 0x829 && kind == 0 && !w->sdregs) { w->sdregs = address; w->sd_revision = core_revision; }
        }
    }
    if (!ended || !w->arm || !w->arm_wrap || !w->d11_wrap || !w->sdregs) return HW_PROTOCOL;
    if (core_reset(w, w->arm_wrap, 0x20, 0x20) != HW_OK ||
        core_disable(w, w->d11_wrap, 12, 4) != HW_OK) return HW_IO;
    if (read32(w, w->arm + 4, &value) != HW_OK) return HW_IO;
    unsigned int banks = (value & 15U) + ((value >> 4) & 15U);
    if (!banks || banks > 16) return HW_UNSUPPORTED;
    for (unsigned int i = 0; i < banks; i++) {
        if (write32(w, w->arm + 0x40, i) != HW_OK || read32(w, w->arm + 0x44, &value) != HW_OK) return HW_IO;
        w->ram_bytes += ((value & 127U) + 1) * ((value & 512U) ? 1024U : 8192U);
    }
    if (w->ram_bytes < 0x20000 || w->ram_bytes > 0x200000) return HW_UNSUPPORTED;
    return HW_OK;
}

static int fifo(Wifi* w, int write, unsigned char* data, unsigned int bytes) {
    while (bytes) {
        unsigned int count = bytes > 512 ? 512 : bytes;
        int result = w->bus.transfer(w->bus.context, 2, 0, 0, write, data, count);
        if (result != HW_OK) return result;
        data += count; bytes -= count;
    }
    return HW_OK;
}

static void event(Wifi* w, const unsigned char* p, unsigned int bytes) {
    /* Ethernet + Broadcom vendor header + versioned big-endian wl_event_msg. */
    if (bytes < 72 || p[12] != 0x88 || p[13] != 0x6c || p[19] != 0 || p[20] != 0x10 || p[21] != 0x18 ||
        p[22] != 0 || p[23] != 1 || p[24] != 0 || p[25] != 2 || p[70] || p[71]) return;
    const unsigned char* message = p + 24;
    uint32_t type = be32(message + 4), status = be32(message + 8), length = be32(message + 20);
    if (length > bytes - 72) return;
    if (type == 16) {
        w->link = !!(message[3] & 1U);
        if (!w->link) { w->keyed = w->connected = 0; w->rx_count = w->rx_head = 0; }
    } else if (type == 46) {
        if (status == 6) w->keyed = 1;
        else if (status == 7) { w->keyed = w->connected = 0; w->join_error = 1; }
    } else if (type == 0) {
        if (status) { w->join_error = 1; w->connected = 0; }
        else w->associated = 1;
    }
    else if (type == 5 || type == 6 || type == 11 || type == 12) {
        w->link = w->keyed = w->connected = 0; w->rx_count = w->rx_head = 0;
    }
}

static int pump(Wifi* w) {
    uint32_t interrupts, mailbox;
    if (read32(w, w->sdregs + 0x20, &interrupts) != HW_OK) return HW_IO;
    w->host_flow = !!(interrupts & 16U);
    if (interrupts && write32(w, w->sdregs + 0x20, interrupts) != HW_OK) return HW_IO;
    if (interrupts & 128U) {
        if (read32(w, w->sdregs + 0x4c, &mailbox) != HW_OK || write32(w, w->sdregs + 0x40, 2) != HW_OK) return HW_IO;
        if ((mailbox & 0x88U) && ((mailbox >> 16) & 255U) != 4) return HW_UNSUPPORTED;
        if (mailbox & 0x88U) w->protocol_ready = 1;
    }
    uint8_t low, high;
    if (reg8(w, 1, 0x1001b, 0, &low) != HW_OK || reg8(w, 1, 0x1001c, 0, &high) != HW_OK) return HW_IO;
    if (!low && !high) return 0;
    if (fifo(w, 0, w->rx, 12) != HW_OK) return HW_IO;
    unsigned int length = le16(w->rx), complement = le16(w->rx + 2);
    if (!length) return 0;
    if ((length ^ complement) != 0xffffU || length < 12 || length > sizeof(w->rx) ||
        w->rx[7] < 12 || w->rx[7] > length || w->rx[4] != w->rx_sequence) return HW_PROTOCOL;
    if (length > 12 && fifo(w, 0, w->rx + 12, (length - 12 + 3U) & ~3U) != HW_OK) return HW_IO;
    w->rx_sequence++;
    if ((uint8_t)(w->rx[9] - w->tx_sequence) > 64U) return HW_PROTOCOL;
    w->tx_window = w->rx[9]; w->flow_mask = w->rx[8];
    unsigned int channel = w->rx[5] & 15U, offset = w->rx[7];
    unsigned char* p = w->rx + offset;
    unsigned int bytes = length - offset;
    if (channel == 0) {
        if (bytes < 16) return HW_PROTOCOL;
        if ((le32(p + 8) >> 16) != (w->control_id & 65535U)) return HW_OK;
        if (le32(p) != w->control_command) return HW_PROTOCOL;
        w->control_status = le32(p + 12) | (le32(p + 8) & 1U);
        w->control_length = w->control_set || w->control_status ? 0 : le32(p + 4);
        if (w->control_length > bytes - 16 || w->control_length > sizeof(w->control)) return HW_PROTOCOL;
        memcpy(w->control, p + 16, w->control_length);
        w->control_ready = 1;
    } else if (channel == 1 || channel == 2) {
        if (bytes < 4 || (p[0] >> 4) != 2 || (p[2] & 15U) != 0) return HW_PROTOCOL;
        unsigned int skip = 4U + (unsigned int)p[3] * 4U;
        if (skip > bytes) return HW_PROTOCOL;
        p += skip; bytes -= skip;
        if (channel == 1) event(w, p, bytes);
        else if (bytes >= 14 && bytes <= WIFI_FRAME_MAX && w->connected && w->link && w->keyed) {
            if (w->rx_count == 4) w->dropped++;
            else {
                unsigned int slot = (w->rx_head + w->rx_count) % 4;
                memcpy(w->frames[slot], p, bytes); w->frame_bytes[slot] = bytes; w->rx_count++;
            }
        }
    } else return HW_UNSUPPORTED; /* Glom aggregation is disabled during setup. */
    return HW_OK;
}

static int credit(Wifi* w, unsigned int channel) {
    uint32_t start = w->bus.time_us(w->bus.context);
    for (unsigned int i = 0; i < 100000; i++) {
        if (w->tx_sequence != w->tx_window && (!channel || (!w->host_flow && !(w->flow_mask & 1U)))) return HW_OK;
        int result = pump(w);
        if (result < 0) return result;
        if (w->bus.time_us(w->bus.context) - start >= 2500000U) break;
    }
    return HW_TIMEOUT;
}

static void header(Wifi* w, unsigned int bytes, unsigned int channel) {
    put16(w->tx, bytes); put16(w->tx + 2, bytes ^ 65535U);
    w->tx[4] = w->tx_sequence; w->tx[5] = (unsigned char)channel; w->tx[7] = 12;
}

static int control(Wifi* w, uint32_t command, int set, const void* input, unsigned int input_bytes,
                    void* output, unsigned int capacity, unsigned int* output_bytes) {
    unsigned int payload = input_bytes > capacity ? input_bytes : capacity;
    if (payload > sizeof(w->control) || (input_bytes && !input) || (capacity && !output)) return HW_INVALID;
    int result = credit(w, 0);
    if (result != HW_OK) return fail(w, result);
    memset(w->tx, 0, (payload + 28 + 3U) & ~3U);
    w->control_id = (w->control_id + 1) & 65535U;
    w->control_command = command; w->control_ready = 0; w->control_set = set;
    header(w, payload + 28, 0);
    put32(w->tx + 12, command); put32(w->tx + 16, payload);
    put32(w->tx + 20, (w->control_id << 16) | (set ? 2U : 0U));
    if (input_bytes) memcpy(w->tx + 28, input, input_bytes);
    if (set && command == 26) w->link = w->keyed = w->associated = w->join_error = 0;
    result = fifo(w, 1, w->tx, (payload + 28 + 3U) & ~3U);
    erase(w->tx, sizeof(w->tx));
    if (result != HW_OK) return fail(w, result);
    w->tx_sequence++;
    uint32_t start = w->bus.time_us(w->bus.context);
    for (unsigned int i = 0; !w->control_ready && i < 100000; i++) {
        result = pump(w);
        if (result < 0) return fail(w, result);
        if (w->bus.time_us(w->bus.context) - start >= 2500000U) return fail(w, HW_TIMEOUT);
    }
    if (!w->control_ready) return fail(w, HW_TIMEOUT);
    erase(w->rx, sizeof(w->rx));
    if (w->control_status) { erase(w->control, sizeof(w->control)); return HW_IO; }
    if (!set && w->control_length > capacity) { erase(w->control, sizeof(w->control)); return HW_NO_SPACE; }
    if (!set && w->control_length) memcpy(output, w->control, w->control_length);
    if (output_bytes) *output_bytes = set ? 0 : w->control_length;
    erase(w->control, sizeof(w->control));
    return HW_OK;
}

static int variable(Wifi* w, const char* name, int set, const void* value, unsigned int bytes,
                     void* output, unsigned int capacity, unsigned int* actual) {
    unsigned char request[1536];
    unsigned int name_bytes = 0;
    while (name_bytes < 63 && name[name_bytes]) name_bytes++;
    if (name_bytes == 63 || name_bytes + 1 + bytes > sizeof(request)) return HW_INVALID;
    memcpy(request, name, name_bytes + 1);
    if (bytes) memcpy(request + name_bytes + 1, value, bytes);
    int result = control(w, set ? 263U : 262U, set, request, name_bytes + 1 + bytes, output, capacity, actual);
    erase(request, sizeof(request));
    return result;
}
static int var_int(Wifi* w, const char* name, uint32_t value) {
    unsigned char data[4]; put32(data, value);
    return variable(w, name, 1, data, 4, NULL, 0, NULL);
}
static int command_int(Wifi* w, uint32_t command, uint32_t value) {
    unsigned char data[4]; put32(data, value);
    return control(w, command, 1, data, 4, NULL, 0, NULL);
}

int wifi_boot(Wifi* w, const WifiBus* bus, const WifiFirmware* f) {
    if (!w || !bus || !bus->direct || !bus->transfer || !bus->time_us || !bus->delay || !bus->owned ||
        !bus->owned(bus->context) || !f || !f->firmware || f->firmware_bytes < 4 || f->firmware_bytes > 1048576 ||
        !f->clm || !f->clm_bytes || f->clm_bytes > 262144 || f->country[2] ||
        f->country[0] < 'A' || f->country[0] > 'Z' || f->country[1] < 'A' || f->country[1] > 'Z' ||
        f->country_revision < -1) return HW_INVALID;
    if (w->state) return HW_BUSY;
    w->bus = *bus; w->state = 2; w->window = ~0U; w->tx_window = 1; w->secure = 1;
    size_t nvram_bytes = 0;
    if (wifi_nvram_pack(f->nvram, f->nvram_bytes, w->nvram, sizeof(w->nvram), &nvram_bytes) != HW_OK)
        return fail(w, HW_INVALID);
    if (set8(w, 1, CLOCK_CSR, 0x28) != HW_OK || wait8(w, 1, CLOCK_CSR, 64, 64, 500000) != HW_OK ||
        set8(w, 1, CLOCK_CSR, 0x21) != HW_OK || bus->delay(bus->context, 65) != HW_OK) return fail(w, HW_TIMEOUT);
    int result = chip_probe(w);
    if (result != HW_OK) return fail(w, result);
    if (w->sd_revision >= 12) {
        uint8_t sleep;
        if (reg8(w, 1, 0x1001f, 0, &sleep) != HW_OK || set8(w, 1, 0x1001f, sleep | 1U) != HW_OK)
            return fail(w, HW_IO);
    }
    if (f->firmware_bytes + nvram_bytes + 4 > w->ram_bytes) return fail(w, HW_NO_SPACE);
    if (write32(w, RAM_BASE + w->ram_bytes - 4, 0) != HW_OK ||
        backplane(w, RAM_BASE, (void*)f->firmware, (unsigned int)f->firmware_bytes, 1) != HW_OK)
        return fail(w, HW_IO);
    /* Read back every downloaded byte before releasing the radio CPU. */
    for (unsigned int offset = 0; offset < f->firmware_bytes; offset += 512) {
        unsigned int count = (unsigned int)f->firmware_bytes - offset;
        if (count > 512) count = 512;
        if (backplane(w, RAM_BASE + offset, w->rx, count, 0) != HW_OK ||
            memcmp(w->rx, f->firmware + offset, count)) return fail(w, HW_IO);
    }
    uint32_t nvram_start = RAM_BASE + w->ram_bytes - 4 - (uint32_t)nvram_bytes;
    uint32_t words = (uint32_t)nvram_bytes / 4;
    if (backplane(w, nvram_start, w->nvram, (unsigned int)nvram_bytes, 1) != HW_OK ||
        write32(w, RAM_BASE + w->ram_bytes - 4, words | ((~words & 65535U) << 16)) != HW_OK)
        return fail(w, HW_IO);
    for (unsigned int offset = 0; offset < nvram_bytes; offset += 512) {
        unsigned int count = (unsigned int)nvram_bytes - offset;
        if (count > 512) count = 512;
        if (backplane(w, nvram_start + offset, w->rx, count, 0) != HW_OK ||
            memcmp(w->rx, w->nvram + offset, count)) return fail(w, HW_IO);
    }
    uint32_t token;
    if (read32(w, RAM_BASE + w->ram_bytes - 4, &token) != HW_OK || token != (words | ((~words & 65535U) << 16)) ||
        write32(w, w->sdregs + 0x20, ~0U) != HW_OK || !le32(f->firmware) ||
        write32(w, 0, le32(f->firmware)) != HW_OK || core_reset(w, w->arm_wrap, 0x20, 0) != HW_OK)
        return fail(w, HW_IO);
    if (set8(w, 1, CLOCK_CSR, 0x10) != HW_OK || wait8(w, 1, CLOCK_CSR, 128, 128, 1000000) != HW_OK ||
        set8(w, 1, CLOCK_CSR, 2) != HW_OK || write32(w, w->sdregs + 0x48, 4U << 16) != HW_OK ||
        set8(w, 0, 2, 6) != HW_OK || wait8(w, 0, 3, 6, 6, 1000000) != HW_OK ||
        write32(w, w->sdregs + 0x24, 0xf0) != HW_OK || set8(w, 0, 4, 7) != HW_OK ||
        set8(w, 1, 0x10008, 0x60) != HW_OK || set8(w, 1, 0x1001d, 0xd0) != HW_OK)
        return fail(w, HW_IO);
    uint8_t device;
    if (reg8(w, 1, 0x10009, 0, &device) != HW_OK || set8(w, 1, 0x10009, device | 16U) != HW_OK)
        return fail(w, HW_IO);
    uint32_t ready_start = bus->time_us(bus->context);
    for (unsigned int attempt = 0; !w->protocol_ready; attempt++) {
        result = pump(w);
        if (result < 0) return fail(w, result);
        if (attempt == 100000 || bus->time_us(bus->context) - ready_start >= 1000000U)
            return fail(w, HW_TIMEOUT);
    }
    for (size_t offset = 0; offset < f->clm_bytes;) {
        unsigned char chunk[1040] = {0};
        unsigned int count = (unsigned int)(f->clm_bytes - offset);
        if (count > 1024) count = 1024;
        unsigned int flags = 0x1000U | (!offset ? 2U : 0U) | (offset + count == f->clm_bytes ? 4U : 0U);
        put16(chunk, flags); put16(chunk + 2, 2); put32(chunk + 4, count);
        memcpy(chunk + 12, f->clm + offset, count);
        result = variable(w, "clmload", 1, chunk, 12 + count, NULL, 0, NULL);
        if (result != HW_OK) return fail(w, result);
        offset += count;
    }
    unsigned char country[12] = {0}, applied_country[12] = {0}, status[4] = {0};
    country[0] = country[8] = (unsigned char)f->country[0];
    country[1] = country[9] = (unsigned char)f->country[1]; put32(country + 4, (uint32_t)f->country_revision);
    unsigned int actual = 0;
    if (variable(w, "clmload_status", 0, NULL, 0, status, 4, &actual) != HW_OK || actual != 4 || le32(status) ||
        variable(w, "country", 1, country, sizeof(country), NULL, 0, NULL) != HW_OK ||
        variable(w, "country", 0, NULL, 0, applied_country, sizeof(applied_country), &actual) != HW_OK || actual != 12 ||
        applied_country[0] != country[0] || applied_country[1] != country[1] ||
        applied_country[8] != country[8] || applied_country[9] != country[9] ||
        (f->country_revision >= 0 && le32(applied_country + 4) != (uint32_t)f->country_revision) ||
        var_int(w, "bus:txglom", 0) != HW_OK || command_int(w, 86, 0) != HW_OK ||
        command_int(w, 20, 1) != HW_OK || var_int(w, "roam_off", 1) != HW_OK) return fail(w, HW_IO);
    unsigned char mask[16] = {0};
    const unsigned int events[] = {0, 5, 6, 11, 12, 16, 46};
    for (unsigned int i = 0; i < sizeof(events) / sizeof(events[0]); i++) mask[events[i] / 8] |= (uint8_t)(1U << (events[i] % 8));
    if (variable(w, "event_msgs", 1, mask, sizeof(mask), NULL, 0, NULL) != HW_OK ||
        variable(w, "cur_etheraddr", 0, NULL, 0, w->mac, 6, &actual) != HW_OK || actual != 6 ||
        (w->mac[0] & 1) || !(w->mac[0] | w->mac[1] | w->mac[2] | w->mac[3] | w->mac[4] | w->mac[5]) ||
        command_int(w, 2, 1) != HW_OK) return fail(w, HW_IO);
    erase(w->nvram, sizeof(w->nvram)); w->state = 1; w->error = 0;
    return HW_OK;
}

static int enter(Wifi* w) {
    if (!w || !w->bus.owned || !w->bus.owned(w->bus.context)) return HW_INVALID;
    if (w->state != 1) return HW_OFF;
    if (w->busy) return HW_BUSY;
    w->busy = 1;
    return HW_OK;
}
int wifi_poll(Wifi* w) {
    int result = enter(w);
    if (result != HW_OK) return result;
    result = pump(w);
    w->busy = 0;
    return result < 0 ? fail(w, result) : result;
}

int wifi_join(Wifi* w, const unsigned char* ssid, unsigned int ssid_bytes,
               const unsigned char* passphrase, unsigned int passphrase_bytes) {
    if (!ssid || !ssid_bytes || ssid_bytes > 32 || !passphrase || passphrase_bytes < 8 || passphrase_bytes > 63)
        return HW_INVALID;
    for (unsigned int i = 0; i < passphrase_bytes; i++) if (passphrase[i] < 32 || passphrase[i] > 126) return HW_INVALID;
    int result = enter(w);
    if (result != HW_OK) return result;
    w->link = w->keyed = w->connected = 0; w->secure = 1; w->join_error = 0; w->rx_count = w->rx_head = 0;
    unsigned char pmk[68] = {0}, target[36] = {0};
    static const unsigned char rsn[] = {0x30, 0x14, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4,
                                        1, 0, 0, 0x0f, 0xac, 2, 0, 0};
    put16(pmk, passphrase_bytes); put16(pmk + 2, 1); memcpy(pmk + 4, passphrase, passphrase_bytes);
    put32(target, ssid_bytes); memcpy(target + 4, ssid, ssid_bytes);
    if (control(w, 52, 1, NULL, 0, NULL, 0, NULL) != HW_OK ||
        command_int(w, 22, 0) != HW_OK || command_int(w, 134, 4) != HW_OK || var_int(w, "wpa_auth", 128) != HW_OK ||
        variable(w, "wpaie", 1, rsn, sizeof(rsn), NULL, 0, NULL) != HW_OK || var_int(w, "sup_wpa", 1) != HW_OK ||
        control(w, 268, 1, pmk, sizeof(pmk), NULL, 0, NULL) != HW_OK) result = HW_IO;
    else {
        /* Configuration commands may have drained events from a previous
         * association. Only events after this request can satisfy this join. */
        for (unsigned int drained = 0; drained < 64; drained++) {
            int old = pump(w);
            if (old < 0) { result = fail(w, old); goto join_done; }
            if (!old) break;
            if (drained == 63) { result = HW_BUSY; goto join_done; }
        }
        w->link = w->keyed = w->associated = w->join_error = 0;
        if (control(w, 26, 1, target, sizeof(target), NULL, 0, NULL) != HW_OK) {
            result = HW_IO;
            goto join_done;
        }
        uint32_t start = w->bus.time_us(w->bus.context);
        result = HW_TIMEOUT;
        for (unsigned int i = 0; i < 1000000U && !w->join_error; i++) {
            int polled = pump(w);
            if (polled < 0) { result = fail(w, polled); break; }
            if (w->associated && w->link && w->keyed) {
                unsigned char current[36] = {0}; unsigned int actual;
                result = control(w, 25, 0, NULL, 0, current, sizeof(current), &actual);
                if (result == HW_OK && w->associated && w->link && w->keyed && !w->join_error &&
                    actual == sizeof(current) && le32(current) == ssid_bytes &&
                    !memcmp(current + 4, ssid, ssid_bytes)) { w->connected = 1; break; }
                result = HW_PROTOCOL;
                break;
            }
            if (w->bus.time_us(w->bus.context) - start >= 30000000U) break;
        }
        if (w->join_error) result = HW_IO;
    }
join_done:
    erase(pmk, sizeof(pmk)); erase(w->tx, sizeof(w->tx)); erase(w->control, sizeof(w->control));
    if (result != HW_OK) {
        w->link = w->keyed = w->connected = 0;
        if (w->state == 1) (void)control(w, 52, 1, NULL, 0, NULL, 0, NULL);
    }
    w->busy = 0;
    return result;
}

int wifi_disconnect(Wifi* w) {
    int result = enter(w);
    if (result != HW_OK) return result;
    result = control(w, 52, 1, NULL, 0, NULL, 0, NULL);
    w->link = w->keyed = w->connected = 0; w->rx_count = w->rx_head = 0; w->busy = 0;
    return result;
}

int wifi_send(Wifi* w, const void* ethernet, unsigned int bytes) {
    if (!ethernet || bytes < 14 || bytes > WIFI_FRAME_MAX) return HW_INVALID;
    int result = enter(w);
    if (result != HW_OK) return result;
    if (!w->connected || !w->link || !w->keyed) { w->busy = 0; return HW_OFF; }
    result = credit(w, 2);
    if (result == HW_OK && (!w->connected || !w->link || !w->keyed)) result = HW_OFF;
    if (result == HW_OK) {
        memset(w->tx, 0, (bytes + 16 + 3U) & ~3U); header(w, bytes + 16, 2); w->tx[12] = 0x20;
        memcpy(w->tx + 16, ethernet, bytes);
        result = fifo(w, 1, w->tx, (bytes + 16 + 3U) & ~3U);
        if (result == HW_OK) w->tx_sequence++;
    }
    w->busy = 0;
    erase(w->tx, sizeof(w->tx));
    return result < 0 && result != HW_OFF ? fail(w, result) : result;
}

int wifi_receive(Wifi* w, void* ethernet, unsigned int capacity) {
    if (!ethernet) return HW_INVALID;
    int result = enter(w);
    if (result != HW_OK) return result;
    result = 0;
    if (w->rx_count) {
        unsigned int bytes = w->frame_bytes[w->rx_head];
        if (capacity < bytes) result = HW_NO_SPACE;
        else {
            memcpy(ethernet, w->frames[w->rx_head], bytes); result = (int)bytes;
            w->rx_head = (w->rx_head + 1) % 4; w->rx_count--;
        }
    }
    w->busy = 0;
    return result;
}
