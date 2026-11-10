#include <vantage/drivers/wifi.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define ENUM 0x18000000U
#define SD (ENUM + 0x3000U)
#define ARM (ENUM + 0x2000U)
#define WRAP (ENUM + 0x102000U)
static Wifi wifi;
typedef struct {
    unsigned char registers[0x200000], ram[0x80000], fn0[512], fn1[0x10020], reset[4];
    unsigned char incoming[32][WIFI_PACKET_MAX], outgoing[WIFI_PACKET_MAX];
    unsigned int incoming_bytes[32], head, queued, offset, sent;
    uint32_t clock, window;
    unsigned int sequence, uploads, released, country, commands, joins;
    unsigned char ssid[36], country_data[12];
    int owner, mode, reject_wpa, stale_events, no_join_events;
} Radio;
static Radio radio;
static uint32_t get32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static unsigned int get16(const unsigned char* p) { return (unsigned int)p[0] | ((unsigned int)p[1] << 8); }
static void put16(unsigned char* p, unsigned int n) { p[0] = (unsigned char)n; p[1] = (unsigned char)(n >> 8); }
static void put32(unsigned char* p, uint32_t n) { for (unsigned int i = 0; i < 4; i++) p[i] = (unsigned char)(n >> (i * 8)); }
static void putbe(unsigned char* p, uint32_t n) { for (unsigned int i = 0; i < 4; i++) p[3 - i] = (unsigned char)(n >> (i * 8)); }
static unsigned char* bp(uint32_t address, unsigned int bytes) {
    if (address == 0) { CHECK(bytes == 4); return radio.reset; }
    if (address >= 0x198000U && address < 0x218000U) {
        CHECK(bytes <= 0x218000U - address); return radio.ram + address - 0x198000U;
    }
    CHECK(address >= ENUM && address < ENUM + sizeof(radio.registers));
    CHECK(bytes <= ENUM + sizeof(radio.registers) - address);
    return radio.registers + address - ENUM;
}
static void enqueue(unsigned int channel, const unsigned char* payload, unsigned int bytes) {
    CHECK(radio.queued < 32 && bytes + 12 <= WIFI_PACKET_MAX);
    unsigned int slot = (radio.head + radio.queued) % 32;
    unsigned char* p = radio.incoming[slot]; memset(p, 0, WIFI_PACKET_MAX);
    put16(p, bytes + 12); put16(p + 2, (bytes + 12) ^ 65535U);
    p[4] = (unsigned char)radio.sequence++; p[5] = (unsigned char)channel; p[7] = 12;
    p[9] = (unsigned char)(wifi.tx_sequence + 8U);
    memcpy(p + 12, payload, bytes);
    radio.incoming_bytes[slot] = (bytes + 12 + 3U) & ~3U; radio.queued++;
}
static void event(unsigned int type, unsigned int status, unsigned int flags) {
    unsigned char p[76] = {0}; p[0] = 0x20; /* BDC */
    unsigned char* frame = p + 4;
    frame[12] = 0x88; frame[13] = 0x6c; frame[14] = 0x80; frame[15] = 1;
    frame[20] = 0x10; frame[21] = 0x18; frame[23] = 1; frame[25] = 2; frame[27] = (unsigned char)flags;
    putbe(frame + 28, type); putbe(frame + 32, status);
    enqueue(1, p, sizeof(p));
}
static void transmit(void) {
    unsigned char* p = radio.outgoing;
    unsigned int length = get16(p);
    CHECK((length ^ get16(p + 2)) == 65535U && p[7] == 12);
    for (unsigned int i = length; i < radio.sent; i++) CHECK(!p[i]);
    if (p[5] == 2) { CHECK(p[12] == 0x20 && length <= WIFI_FRAME_MAX + 16); return; }
    CHECK(p[5] == 0 && length >= 28);
    radio.commands++;
    unsigned char response[4096 + 16] = {0};
    uint32_t command = get32(p + 12), flags = get32(p + 20), payload = get32(p + 16);
    CHECK(payload == length - 28);
    put32(response, command); put32(response + 8, flags);
    unsigned int bytes = 0;
    if (command == 262) {
        if (!strcmp((const char*)p + 28, "cur_etheraddr")) {
            const unsigned char mac[6] = {2, 0, 0, 0, 0, 1}; memcpy(response + 16, mac, 6); bytes = 6;
        } else if (!strcmp((const char*)p + 28, "clmload_status")) bytes = 4;
        else if (!strcmp((const char*)p + 28, "country")) { memcpy(response + 16, radio.country_data, 12); bytes = 12; }
        else CHECK(0);
    } else if (command == 263) {
        const char* name = (const char*)p + 28;
        if (!strcmp(name, "country")) {
            CHECK(p[28 + 8] == 'G' && p[28 + 9] == 'B'); radio.country = 1;
            memcpy(radio.country_data, p + 28 + 8, 12);
        }
        if (!strcmp(name, "sup_wpa") && radio.reject_wpa) put32(response + 12, 23);
        if (!strcmp(name, "clmload")) {
            const unsigned char* chunk = p + 28 + sizeof("clmload");
            CHECK((get16(chunk) & 0x1000U) && get16(chunk + 2) == 2 && get32(chunk + 4) <= 1024);
        }
    } else if (command == 26) {
        CHECK(radio.country); memcpy(radio.ssid, p + 28, 36); radio.joins++;
    } else if (command == 25) { memcpy(response + 16, radio.ssid, 36); bytes = 36; }
    else if (command == 2) CHECK(radio.country);
    else if (command == 268) CHECK(get16(p + 28) == 8 && get16(p + 30) == 1);
    /* SET replies can echo the original length without returning a payload. */
    put32(response + 4, (flags & 2) ? payload : bytes);
    if (radio.mode != 1) enqueue(0, response, bytes + 16);
    if (command == 26 && radio.mode != 1 && !radio.no_join_events) { event(0, 0, 0); event(16, 0, 1); event(46, 6, 0); }
    if (command == 52 && radio.stale_events) { event(0, 0, 0); event(16, 0, 1); event(46, 6, 0); }
}
static int direct(void* context, unsigned int fn, uint32_t address, int write, uint8_t* value) {
    (void)context; CHECK(fn <= 1);
    unsigned char* registers = fn ? radio.fn1 : radio.fn0;
    CHECK(address < (fn ? sizeof(radio.fn1) : sizeof(radio.fn0)));
    if (fn && address < 0x10000) {
        unsigned char* data = bp((radio.window & ~0x7fffU) | (address & 0x7fffU), 1);
        if (write) *data = *value; else *value = *data;
        return HW_OK;
    }
    if (write) {
        registers[address] = *value;
        if (fn && address >= 0x1000a && address <= 0x1000c)
            radio.window = ((uint32_t)radio.fn1[0x1000a] << 8) | ((uint32_t)radio.fn1[0x1000b] << 16) |
                           ((uint32_t)radio.fn1[0x1000c] << 24);
    } else if (fn && address == 0x1000e) *value = radio.fn1[address] | 0xc0;
    else if (!fn && address == 3) *value = radio.fn0[2];
    else if (fn && (address == 0x1001b || address == 0x1001c)) {
        unsigned int bytes = radio.queued ? radio.incoming_bytes[radio.head] : 0;
        *value = (unsigned char)(bytes >> (address == 0x1001b ? 0 : 8));
    } else *value = registers[address];
    return HW_OK;
}
static int transfer(void* context, unsigned int fn, uint32_t address, int increment, int write,
                      void* memory, unsigned int bytes) {
    (void)context; CHECK(bytes && bytes <= 512);
    if (fn == 1) {
        CHECK(increment && (address & 0x8000));
        uint32_t absolute = (radio.window & ~0x7fffU) | (address & 0x7fffU);
        unsigned char* target = bp(absolute, bytes);
        if (write) {
            if (absolute == SD + 0x20) memset(target, 0, bytes);
            else memcpy(target, memory, bytes);
            if (absolute >= 0x198000U && absolute < 0x218000U) radio.uploads++;
            if (absolute == WRAP + 0x408 && get32(target) == 1) {
                radio.released++; put32(bp(SD + 0x20, 4), 128); put32(bp(SD + 0x4c, 4), 0x40088);
            }
        } else {
            memcpy(memory, target, bytes);
            if (radio.mode == 2 && absolute == 0x198000U) ((unsigned char*)memory)[0] ^= 1;
        }
    } else {
        CHECK(fn == 2 && !address && !increment);
        if (write) {
            CHECK(radio.sent + bytes <= sizeof(radio.outgoing));
            memcpy(radio.outgoing + radio.sent, memory, bytes); radio.sent += bytes;
            if (radio.sent >= ((get16(radio.outgoing) + 3U) & ~3U)) { transmit(); radio.sent = 0; }
        } else {
            CHECK(radio.queued && bytes <= radio.incoming_bytes[radio.head] - radio.offset);
            memcpy(memory, radio.incoming[radio.head] + radio.offset, bytes); radio.offset += bytes;
            if (radio.offset == radio.incoming_bytes[radio.head]) {
                radio.offset = 0; radio.head = (radio.head + 1) % 32; radio.queued--;
            }
        }
    }
    return HW_OK;
}
static uint32_t clock_us(void* context) { (void)context; radio.clock += 1000; return radio.clock; }
static int delay(void* context, uint32_t us) { (void)context; radio.clock += us; return HW_OK; }
static int owner(void* context) { (void)context; return radio.owner; }
static void initialize(int mode) {
    memset(&radio, 0, sizeof(radio)); memset(&wifi, 0, sizeof(wifi)); radio.owner = 1; radio.mode = mode;
    put32(bp(ENUM, 4), 0x10064345U); put32(bp(ENUM + 0xfc, 4), ENUM + 0x100000U);
    put32(bp(ARM + 4, 4), 1); put32(bp(ARM + 0x44, 4), 63);
    const uint32_t cores[] = {0x83e, 0x812, 0x829};
    const uint32_t bases[] = {ARM, ENUM + 0x1000U, SD};
    const uint32_t wraps[] = {WRAP, ENUM + 0x101000U, ENUM + 0x103000U};
    unsigned int offset = 0;
    for (unsigned int i = 0; i < 3; i++) {
        uint32_t words[] = {(cores[i] << 8) | 1U, (12U << 24) | (1U << 14) | 1U, 3, bases[i] | 5U, wraps[i] | 0xc5U};
        for (unsigned int j = 0; j < 5; j++, offset += 4) put32(bp(ENUM + 0x100000U + offset, 4), words[j]);
    }
    put32(bp(ENUM + 0x100000U + offset, 4), 15);
}
static int boot(void) {
    static const unsigned char firmware[1024] = {1, 2, 3, 4};
    static const unsigned char nvram[] = "# fixture\nboardtype=0x1\nregrev=0\n";
    static const unsigned char clm[2051] = {0};
    WifiFirmware f = {firmware, sizeof(firmware), nvram, sizeof(nvram) - 1, clm, sizeof(clm), "GB", 0};
    WifiBus bus = {NULL, direct, transfer, clock_us, delay, owner};
    return wifi_boot(&wifi, &bus, &f);
}
int main(void) {
    unsigned char packed[64] = {0}; size_t packed_size = 0;
    static const unsigned char invalid[] = {'a', '=', '1', 0, 'x'};
    CHECK(wifi_nvram_pack(invalid, sizeof(invalid), packed, sizeof(packed), &packed_size) == HW_INVALID);
    const unsigned char nv[] = " # c\r\n a=1 # hi\n b=2\n";
    CHECK(wifi_nvram_pack(nv, sizeof(nv) - 1, packed, sizeof(packed), &packed_size) == HW_OK);
    CHECK(packed_size == 12 && !memcmp(packed, "a=1\0b=2\0\0\0\0\0", 12));
    CHECK(wifi_nvram_pack((const unsigned char*)"bad key=1", 9, packed, sizeof(packed), &packed_size) == HW_INVALID);
    initialize(0); CHECK(boot() == HW_OK && wifi.ram_bytes == 0x80000 && radio.released == 1 && radio.country);
    CHECK(wifi_join(&wifi, (const unsigned char*)"test", 4, (const unsigned char*)"testpass", 8) == HW_OK);
    CHECK(wifi.link && wifi.keyed && radio.joins == 1);
    unsigned char frame[65]; memset(frame, 0xa5, sizeof(frame));
    CHECK(wifi_send(&wifi, frame, 61) == HW_OK);
    for (unsigned int i = 0; i < sizeof(wifi.tx); i++) CHECK(!wifi.tx[i]);
    unsigned char payload[69] = {0}; payload[0] = 0x20; memcpy(payload + 4, frame, sizeof(frame));
    for (unsigned int i = 0; i < 5; i++) enqueue(2, payload, sizeof(payload));
    for (unsigned int i = 0; i < 5; i++) CHECK(wifi_poll(&wifi) == HW_OK);
    CHECK(wifi.rx_count == 4 && wifi.dropped == 1);
    CHECK(wifi_receive(&wifi, frame, 1) == HW_NO_SPACE && wifi.rx_count == 4);
    for (unsigned int i = 0; i < 4; i++) CHECK(wifi_receive(&wifi, frame, sizeof(frame)) == (int)sizeof(frame));
    CHECK(wifi_receive(&wifi, frame, sizeof(frame)) == 0);
    enqueue(2, payload, sizeof(payload)); event(16, 0, 0);
    CHECK(wifi_poll(&wifi) == HW_OK && wifi.rx_count == 1);
    CHECK(wifi_poll(&wifi) == HW_OK && !wifi.link && !wifi.rx_count);
    for (unsigned int i = 0; i < 260; i++) CHECK(wifi_disconnect(&wifi) == HW_OK);
    CHECK(wifi_send(&wifi, frame, sizeof(frame)) == HW_OFF);
    radio.reject_wpa = 1;
    CHECK(wifi_join(&wifi, (const unsigned char*)"test", 4, (const unsigned char*)"testpass", 8) == HW_IO);
    CHECK(!wifi.link && !wifi.keyed && radio.joins == 1);
    radio.reject_wpa = 0;
    CHECK(wifi_join(&wifi, (const unsigned char*)"test", 4, (const unsigned char*)"testpass", 8) == HW_OK);
    radio.owner = 0; CHECK(wifi_poll(&wifi) == HW_INVALID); radio.owner = 1;
    enqueue(2, payload, sizeof(payload)); radio.incoming[radio.head][2] ^= 1;
    CHECK(wifi_poll(&wifi) == HW_PROTOCOL && wifi.state == -1);
    initialize(2); CHECK(boot() == HW_IO && !radio.released);
    initialize(0); put32(bp(ENUM, 4), 0x10094345U); CHECK(boot() == HW_UNSUPPORTED && !radio.uploads);
    initialize(1); radio.clock = 0xffff0000U; CHECK(boot() == HW_TIMEOUT && wifi.state == -1);
    initialize(0); CHECK(boot() == HW_OK); radio.stale_events = radio.no_join_events = 1;
    CHECK(wifi_join(&wifi, (const unsigned char*)"test", 4, (const unsigned char*)"testpass", 8) == HW_TIMEOUT);
    CHECK(!wifi.connected && wifi_send(&wifi, frame, sizeof(frame)) == HW_OFF);
    puts("PASS: modeled CYW43455 boot/readback, CLM/country, WPA2 offload, frames, wrap, bounds and errors");
    return 0;
}
