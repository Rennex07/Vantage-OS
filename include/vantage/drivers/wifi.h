#pragma once
#include <vantage/drivers/sdio.h>

#define WIFI_FRAME_MAX 1514U
#define WIFI_PACKET_MAX 8192U
#define WIFI_NVRAM_MAX 16384U

typedef struct {
    void* context;
    int (*direct)(void*, unsigned int, uint32_t, int, uint8_t*);
    int (*transfer)(void*, unsigned int, uint32_t, int, int, void*, unsigned int);
    uint32_t (*time_us)(void*);
    int (*delay)(void*, uint32_t);
    int (*owned)(void*);
} WifiBus;

typedef struct {
    const unsigned char* firmware;
    size_t firmware_bytes;
    const unsigned char* nvram;
    size_t nvram_bytes;
    const unsigned char* clm;
    size_t clm_bytes;
    char country[3];
    int32_t country_revision;
} WifiFirmware;

typedef struct {
    WifiBus bus;
    int state, busy, error, link, keyed, secure, join_error, associated, connected;
    uint32_t chip, revision, arm, arm_wrap, d11_wrap, sdregs, sd_revision, ram_bytes;
    uint32_t window, control_id, control_command, control_length, control_status;
    uint8_t tx_sequence, rx_sequence, tx_window, flow_mask, mac[6];
    int control_ready, control_set, host_flow, protocol_ready;
    unsigned int rx_head, rx_count, dropped;
    unsigned int frame_bytes[4];
    unsigned char frames[4][WIFI_FRAME_MAX];
    unsigned char tx[WIFI_PACKET_MAX], rx[WIFI_PACKET_MAX], control[4096];
    unsigned char nvram[WIFI_NVRAM_MAX];
    unsigned char erom[4096];
} Wifi;

WifiBus wifi_sdio_bus(Sdio* host);
int wifi_nvram_pack(const unsigned char* text, size_t bytes, unsigned char* output,
                     size_t capacity, size_t* packed_bytes);
/* CYW43455 only (chip 0x4345 revision 6), polling, firmware WPA2-PSK offload.
 * Buffers and state must be private kernel memory, retained for driver lifetime.
 * Firmware/NVRAM/CLM must match this board; country is explicit, not guessed. */
int wifi_boot(Wifi* wifi, const WifiBus* bus, const WifiFirmware* firmware);
int wifi_poll(Wifi* wifi);
int wifi_join(Wifi* wifi, const unsigned char* ssid, unsigned int ssid_bytes,
               const unsigned char* passphrase, unsigned int passphrase_bytes);
int wifi_disconnect(Wifi* wifi);
int wifi_send(Wifi* wifi, const void* ethernet, unsigned int bytes);
/* Returns byte count, zero when no packet is queued, or a negative HwResult. */
int wifi_receive(Wifi* wifi, void* ethernet, unsigned int capacity);
