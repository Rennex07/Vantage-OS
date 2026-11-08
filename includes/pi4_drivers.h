#pragma once
#include "wifi.h"

void pi4_drivers_start(void);
void pi4_drivers_poll(void);
/* NULL unless explicitly enabled and successfully booted. Kernel-only. */
Wifi* pi4_wifi_device(void);
