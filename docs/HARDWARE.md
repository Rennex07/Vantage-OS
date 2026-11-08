# Experimental Pi 4 GPU and Wi-Fi drivers

These drivers are implemented but **not validated on a physical Pi**. Native
controller/protocol models and linked ARM tests are software checks, not proof
of GPU execution, wireless association or cache coherence. Both drivers are
disabled at boot unless explicitly enabled at build time. TCP/IP is separate.

## Builds

Use a fresh external `BUILD_DIR` whenever changing build options. The existing
GNU AArch64 toolchain builds all driver code in a normal kernel. Opt-in startup:

```sh
make BUILD_DIR=/path/to/v3d-build V3D=1
```

For Wi-Fi, supply matching CYW43455 firmware, Pi 4 Model B NVRAM text and CLM
regulatory data from a trusted source. The files and their redistribution
licenses are not bundled. Choose the country for the board's actual location
and the corresponding firmware regulatory revision explicitly:

```sh
python tools/embed_wifi.py --firmware /path/to/brcmfmac43455-sdio.bin \
  --nvram /path/to/board-matched-nvram.txt --clm /path/to/brcmfmac43455-sdio.clm_blob \
  --country GB --regrev 0 --output /path/to/generated/wifi-assets.c
make BUILD_DIR=/path/to/wifi-build WIFI=1 WIFI_FIRMWARE_C=/path/to/generated/wifi-assets.c
```

The country/revision above are an example, not defaults. `V3D=1` and `WIFI=1`
can be combined. The embedding tool records asset hashes beside its generated
source. Keep generated firmware source, manifests and binaries out of Git;
board NVRAM can contain device-specific data. Retain the applicable vendor
licenses with distributed images. No network credentials are included or logged.

## Ownership and failures

All APIs are kernel-only and require core 0, masked EL1 execution and the existing
identity MMU mapping. Zero-initialize driver contexts and retain them for kernel
lifetime. Enabling a driver asserts exclusive ownership of that controller;
firmware/another OS must not concurrently submit work. Calls do not yield.
Secondary CPU workers must complete before GPU ownership transfers.

`drivers/hw/` implements a serialized, bounded property-mailbox transaction and
cache maintenance. Firmware responses are checked for tag, length and completion.
A submitted request that times out quarantines its DMA buffer until reboot.
Other property users must remain synchronous on core 0, including the display's
existing mailbox calls. Polling waits use unsigned timer subtraction and an
iteration bound if the timer stops. APIs return `HwResult` errors.

## V3D 4.2

`drivers/video/v3d.c` powers/probes the Pi 4 GPU and installs a private 4 MiB
page table. Only a private 4 MiB arena is mapped into GPU virtual memory. The
GPU cannot access the kernel, page table, user memory or firmware framebuffer
through these mappings. Addresses use the Pi 4 SCB identity DMA mapping, not
the older VC4 `0xc0000000` RAM alias. The CPU page tables remain unchanged.

The driver generates bounded binning/rendering lists for two RGBA8 raster
surfaces, supporting full-surface clear and copy. It initializes tile state,
performs the V3D 4.x dummy-store workaround, polls completion counters across
wrap, checks GPU/MMU errors and handles CPU/GPU cache transitions. This is a
2D clear/copy path, not an OpenGL/Vulkan implementation or a shader compiler.

Boot reads back every pixel after clear/copy before enabling accelerated
full-canvas clears. Other shapes and presentation keep the CPU renderer. No
speedup is claimed. On a submission failure, the GPU arena stays quarantined;
the display repaints a separate CPU buffer without reading the failed surface.
There is no runtime device reset/reclamation path yet. Two page runs totaling
8 MiB remain reserved for that boot when the GPU option is enabled.

## CYW43455 Wi-Fi

The supported target is **chip 0x4345, revision 6**, as used on Pi 4 Model B.
Other chip revisions fail explicitly. Firmware must support WPA2 supplicant
offload and the requested control variables; unsupported firmware is an error.

`drivers/wifi/sdio.c` uses the legacy EMMC controller at `0xfe300000` and GPIO
34–39. It queries the source clock, powers WLAN through firmware GPIO 129 and
uses polling CMD52/CMD53 PIO, four-bit bus width and at most 25 MHz. It does not
modify GPIO 48–53, SD slot power/voltage or the EMMC2 controller at `0xfe340000`.

The radio driver discovers backplane cores and CR4 RAM, halts the radio CPU,
packs NVRAM, uploads and reads back firmware/NVRAM, writes the reset vector and
starts firmware. It loads CLM data, checks regulatory status, configures the
explicit country and obtains the MAC address without printing it.

SDPCM/BCDC transport implements control request matching, flow credits, sequence
wrap, checked frame lengths, event decoding and four bounded receive slots.
Malformed framing stops the transport. Glom aggregation is disabled; receiving
an unsupported aggregation channel stops rather than misinterpreting it.

`pi4_wifi_device()` returns the initialized kernel device. `wifi_join` accepts
an explicit 1–32-byte SSID and an 8–63-byte ASCII WPA2-PSK passphrase. It sets
CCMP/RSN parameters, requests firmware key offload and requires both link and
successful key exchange, then verifies the associated SSID. Failed authentication
does not fall back to an open network. Host-side key request buffers are scrubbed.
The driver drains old notifications and resets join state at the SSID request.
Frame transport stays unauthorized until that join has been verified.
`wifi_send`/`wifi_receive` transport Ethernet frames through the firmware BDC
header. Frame polling also runs after presentation.

This stage has no automatic association, scan UI, WPA3, AP mode, roaming,
power-saving policy, socket syscalls or TCP/IP. Booting firmware alone is not
network connectivity. Keep credentials in caller-owned runtime memory.

## Verification

```sh
cmake -S drivers -B /path/to/driver-tests
cmake --build /path/to/driver-tests
ctest --test-dir /path/to/driver-tests --output-on-failure
python -m unittest discover -s tests/tools -v
python tests/arm/pi4_drivers.py /path/to/fresh/kernel8.elf -v
```

The native models decode GPU command packets and model SDHCI, backplane RAM,
firmware responses and wireless events. Tests use synthetic data, not vendor
firmware. They cover successful paths, bounds, ownership, counter/timer wrap,
timeouts, malformed frames, authentication rejection and receive overflow.
They do not model GPU microarchitecture, RF, firmware internals or physical DMA.

For hardware acceptance, retain commit/image/asset hashes, board and firmware
versions and a serial log. Never include credentials or device MACs in public
logs. Validate GPU self-test, color order and sustained mixed CPU/GPU rendering;
compare pixels with the CPU path. Verify bounded failure and CPU fallback in a
disposable build. Separately validate SDIO discovery, firmware readiness, WPA2
join/rejection, link loss and bidirectional Ethernet traffic with a controlled
test peer. TCP/IP testing follows when a network stack is integrated.

## References and licenses

- [Pi 4 bare-metal V3D examples](https://github.com/Random06457/rpi4-gpu-bare-metal-examples/tree/a4f74ff75963cf387fe5a42b6c6aeb2d67923c5f): packet layouts and initialization; [MIT notice](licenses/v3d-examples-MIT.txt).
- [Linux v6.12 V3D](https://github.com/torvalds/linux/tree/v6.12/drivers/gpu/drm/v3d): MMU/cache/register semantics, GPL-2.0-or-later portions used under GPL version 3.
- [Linux v6.12 brcmfmac](https://github.com/torvalds/linux/tree/v6.12/drivers/net/wireless/broadcom/brcm80211/brcmfmac): CYW43455, SDPCM/BCDC and WPA firmware protocol; [ISC notice](licenses/brcmfmac-ISC.txt).
- [BCM2711 device tree](https://github.com/torvalds/linux/blob/v6.12/arch/arm/boot/dts/broadcom/bcm2711.dtsi) and [BCM2711 peripherals](https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf).
