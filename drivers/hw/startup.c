#include "../../includes/pi4_drivers.h"
#include "../../includes/video/v3d.h"
#include "../../includes/video/display.h"
#include "../../includes/memory.h"
#include "../../includes/io.h"

#ifndef VANTAGE_V3D
#define VANTAGE_V3D 0
#endif
#ifndef VANTAGE_WIFI
#define VANTAGE_WIFI 0
#endif

#if VANTAGE_V3D
static V3d gpu;
#endif
#if VANTAGE_WIFI
static Sdio sdio;
static Wifi wifi;
extern const WifiFirmware* pi4_wifi_firmware(void);
#endif

#if VANTAGE_V3D || VANTAGE_WIFI
static void report(const char* prefix, int error) {
    char digits[12]; unsigned int used = 0;
    uint32_t value = (uint32_t)(error < 0 ? -(int64_t)error : error);
    do { digits[used++] = (char)('0' + value % 10); value /= 10; } while (value);
    uart_writeText(prefix);
    if (error < 0) uart_writeText("-");
    while (used) { char one[2] = {digits[--used], 0}; uart_writeText(one); }
    uart_writeText(")\n");
}
#endif

void pi4_drivers_start(void) {
#if VANTAGE_V3D
    void* table = (void*)(uintptr_t)mm_alloc_pages(V3D_TABLE_BYTES / 4096, 16);
    void* arena = (void*)(uintptr_t)mm_alloc_pages(V3D_ARENA_BYTES / 4096, 1);
    int gpu_result = table && arena ? v3d_init(&gpu, pi4_hardware_ops(), table, arena, 800, 480) : HW_NO_SPACE;
    if (gpu_result == HW_OK && (!v3d_self_test(&gpu) || !fb_enable_v3d(&gpu))) gpu_result = HW_IO;
    if (gpu_result == HW_OK)
        uart_writeText("[OK] V3D clear/copy pixel self-test passed; full-canvas clears use V3D\n");
    else report("[ERR] V3D unavailable; CPU rendering retained (", gpu_result);
    /* These page runs remain allocated even on failure: the device might still
     * reference them. Reclaiming them requires a hardware-validated reset path. */
#endif
#if VANTAGE_WIFI
    int wifi_result = sdio_init(&sdio, pi4_hardware_ops());
    if (wifi_result == HW_OK) {
        WifiBus bus = wifi_sdio_bus(&sdio);
        wifi_result = wifi_boot(&wifi, &bus, pi4_wifi_firmware());
        if (wifi_result == HW_OK)
            uart_writeText("[OK] CYW43455 firmware ready; network association not requested\n");
        else report("[ERR] CYW43455 firmware initialization failed (", wifi_result);
    } else report("[ERR] WLAN SDIO initialization failed (", wifi_result);
#endif
}

void pi4_drivers_poll(void) {
#if VANTAGE_WIFI
    if (wifi.state == 1) {
        for (unsigned int i = 0; i < 8; i++) {
            int result = wifi_poll(&wifi);
            if (result < 0) { uart_writeText("[ERR] WLAN transport stopped\n"); break; }
            if (!result) break;
        }
    }
#endif
}

Wifi* pi4_wifi_device(void) {
#if VANTAGE_WIFI
    return wifi.state == 1 ? &wifi : NULL;
#else
    return NULL;
#endif
}
