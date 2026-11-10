# Pi 4 GPU and Wi-Fi drivers

The V3D 4.2 renderer is built in and probed at every boot; a probe that fails
for any reason selects the CPU renderer. The CYW43455 Wi-Fi driver stays opt-in.
**Neither driver is validated on a physical Pi.** Native controller/protocol
models and linked ARM tests are software checks, not proof of GPU execution,
wireless association or cache coherence. TCP/IP is separate.

## Builds

Use a fresh external `BUILD_DIR` after source-layout changes. Build-option
changes are tracked automatically. The existing GNU AArch64 toolchain builds
all driver code in a normal kernel:

```sh
make BUILD_DIR=/path/to/build              # V3D probed at boot, CPU fallback
make BUILD_DIR=/path/to/cpu-build V3D=0    # CPU renderer only
```

`V3D=0` removes the boot-time probe: the image never allocates GPU buffers and
never touches the V3D, bridge or reset registers. The driver code stays linked
in both images.

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

The country/revision above are an example, not defaults. `WIFI=1` can be
combined with either V3D setting. The embedding tool records asset hashes
beside its generated source. Keep generated firmware source, manifests and
binaries out of Git; board NVRAM can contain device-specific data. Retain the
applicable vendor licenses with distributed images. No network credentials are
included or logged.

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

## V3D 4.2 rendering backend

Everything in this section is validated against host models and Unicorn only.
Physical Pi 4 validation is **pending**; see the acceptance checklist and the
open hardware questions below. No performance or hardware-correctness claim is
made. This is a 2D clear/present path, not an OpenGL/Vulkan implementation or
a shader compiler.

### Backend seam and operations

`include/vantage/kernel/render.h` is the one interface between the display
layer and its renderers. `kernel/ui/render.c` owns the active draw surface and
always has the CPU raster path; an accelerated `RenderBackend` supplies a
surface plus `fill` and `present` and answers each request with done, skip
(CPU raster does it on the same surface) or failed. `platform/pi4/display.c`
applies the panel rotation and calls `render_pixel`, `render_fill` and
`render_present`. `platform/pi4/startup.c` probes V3D after the display and
worker cores are up and attaches the backend only after its self-test. The
simulator keeps its own CPU display backend.

| Operation | V3D backend | CPU backend |
|---|---|---|
| Full-surface fill (canvas clear) | One V3D clear job | Four-way `raster_fill_rows` |
| Present to the hidden scanout page | One V3D load/store job, if eligible | Four-way `raster_copy_rows` |
| Partial fills and pixels (lines, circles, text, buttons) | CPU raster on the V3D surface, between jobs | CPU raster |

A present runs on V3D only when the self-test proved it, the firmware pitch is
exactly `width * 4`, and the page is 4 KiB aligned, physically contiguous and
below `0xfc000000`. Otherwise the CPU copies from the V3D surface. Both
renderers store the same 32-bit pixel values unchanged: the clear colour is
the pixel word, and load and store use one RGBA8 raster format. Tests compare
both paths pixel for pixel through the device model.

### Bring-up sequence

`v3d_init` and `v3d_self_test` run these steps in order. Each failure records
its stage and `HwResult`, stops the device and returns.

| Stage | Check | Typical failure |
|---|---|---|
| (arguments) | Core 0, masked EL1, aligned contiguous buffers below `0xfc000000`, frame a whole number of pages. No hardware access. | `HW_INVALID` |
| power domain | Property `0x00038030`, domain 11 on. Response must echo the domain with state bit 0 set and bit 1 clear. | mailbox error, `HW_PROTOCOL` |
| clock | Property `0x00038001`, clock 5 on, validated the same way; then `0x00030002` must report a nonzero rate. | mailbox error, `HW_PROTOCOL` |
| reset bridge | Stop both AXI bridges, assert `PM_V3DRSTN`, wait, release, restart the master then the slave bridge. Both restarts must be acknowledged. | `HW_TIMEOUT` |
| identity | Core `IDENT0` reads "V3D" version 4 and `IDENT1` revision 2; hub `IDENT1` reads 4.2 with one core; hub `IDENT2` reports an MMU. | `HW_UNSUPPORTED` |
| idle check | No compute dispatch and no TFU job pending after reset. | `HW_BUSY` |
| MMU setup | Zero the table, map the arena ranges, mask and clear interrupts, program the table base, abort-on-fault control and scratch page, flush the TLB. Table base and enable bit are read back; no fault bit may be set. | `HW_TIMEOUT`, `HW_IO` |
| clear job / present job | Self-test jobs: clear, present to the hidden scanout page, clear. | `HW_TIMEOUT`, `HW_IO` |
| pixel verification | Every cleared pixel, every presented pixel (surface and scanout page) and the guard page after the surface are read back. | `HW_IO` |

Every wait is bounded: 100 ms per register wait, 500 ms per job phase, 1 s per
property call, each with an iteration bound in case the timer stops. The first
bridge stop of the reset cycle may go unacknowledged on a bridge that never
ran; only the restart must succeed.

Stopping the device (after any failure, or `v3d_stop`): mask interrupts,
request a GMP stop and wait for outstanding AXI transfers, stop the slave and
master bridges, assert `PM_V3DRSTN` and read it back, then turn the firmware
domain off. Steps that address the core are skipped if bring-up never reached
it. The stop is *confirmed* when both bridges acknowledged and the reset bit
reads back asserted.

Boot logs one of:

```text
[OK] V3D self-test passed; V3D backend selected (clear, present)
[OK] V3D self-test passed; V3D backend selected (clear; CPU present)
[ERR] V3D unavailable: <stage> (<HwResult>)
[INFO] CPU rendering backend selected
[INFO] V3D disabled at build time (V3D=0); CPU rendering backend selected
```

### Memory ownership

| Buffer | Allocated by | GPU mapping | Who writes, when | Cache maintenance |
|---|---|---|---|---|
| GPU page table, 4 MiB, 64 KiB aligned | `startup.c`, page allocator | None; its address is the MMU table base | Driver only: at init, and the present-window entries around a present job | Cleaned after every write, then a TLB flush, before any job |
| Arena control lists (bin, render, tile; 4 KiB each) | `startup.c`, one 2 MiB page run, 4 KiB aligned | `0x01000000`, permanent | Driver builds them before a submit; nobody during a job | Cleaned before submit |
| Arena tile allocation (64 KiB) and tile state (128 KiB) | same run | permanent | GPU only | Arena cleaned and invalidated once at init |
| Arena draw surface (up to 1.5 MiB) and guard page | same run | permanent | CPU raster while the device is idle; GPU during a clear job | Clear: clean+invalidate before, invalidate after. Present: clean before |
| Arena MMU scratch page (last page) | same run | Never mapped; target of faulting accesses | Nobody | None |
| Scanout page (present target) | VideoCore firmware; `display.c` picks the hidden page | `0x01400000`, only for that present job, then unmapped and the TLB flushed | GPU during the job; CPU only when present runs on the CPU | Clean+invalidate before, invalidate after (the CPU mapping is non-cached) |
| CPU back buffer | `fb_init`, kernel heap | Never | CPU; standby while V3D draws | None |

The gaps between arena ranges and the rest of the 4 GiB GPU space stay
unmapped, so the device cannot reach the kernel, user memory, its own page
table or the visible scanout pages. The CPU uses an identity mapping, so the
physical address equals the pointer; V3D is on the SCB bus with identity DMA
addresses, not the VC4 `0xc0000000` alias (the display masks that alias off
the firmware framebuffer address). A page-table entry is the page frame number
plus valid and writeable bits. The CPU page tables are unchanged.

Lifetime: the table and arena are lent to the driver for one bring-up. When
the driver ends in `V3D_STOPPED` (the table address was never programmed, or
the stop was confirmed) `startup.c` frees both runs. When it ends in
`V3D_QUARANTINED` (stop unconfirmed) both stay reserved until reboot and the
UART says so. A failed allocation frees the other run and selects the CPU
renderer. The CPU back buffer is never freed, so a runtime fallback cannot
fail for lack of memory. A context is never restarted.

### Synchronization and failure handling

- Every display operation runs on core 0 at EL1 with all DAIF masks set, from
  entry to return. The engine checks this before each job and the renderer
  halts on re-entry. Jobs are synchronous: submit, bounded poll, return. No
  job is in flight outside a driver call.
- CPU raster batches (`parallel_run`) join all workers before returning, and
  no job is submitted from inside a batch. Workers and the device therefore
  never touch the surface at the same time, and the CPU never touches a
  buffer while a job that reads or writes it is in flight.
- Sections that must not be preempted or re-entered once timer preemption
  exists: each `render_pixel`/`render_fill`/`render_present` call (including
  the V3D job, its cache/TLB maintenance and any raster batch), `fb_swap`
  through the page flip, and each property-mailbox transaction. If a future
  scheduler runs display syscalls with interrupts unmasked, the engine
  refuses the job (`HW_INVALID`) and the operation runs on the CPU; it must
  keep these sections masked or add its own serialization.
- Completion is polled: the 8-bit bin and render frame counters, compared
  for change so wrap is harmless. The V3D interrupt line is not used and all
  interrupts stay masked. Each poll also checks the hub MMU interrupt bits,
  the MMU control fault bits and the core out-of-memory and GMP-violation
  bits. `V3D_ERR_STAT` is logged after a failure but never decides one.
- No retry. The first timeout or fault stops the device as described above
  and the backend stays off until reboot. The renderer then copies the
  surface to the CPU back buffer, repeats the failed operation on the CPU and
  continues there: a failed clear is redone over every pixel, a failed
  present is redone from the intact surface to the same hidden page. The
  frame is neither lost nor shown partially. The UART logs
  `[ERR] V3D stopped during <stage> (<HwResult>)` and
  `[INFO] CPU rendering backend selected; current frame kept`.

### Physical acceptance checklist

Pending. Keep each result tied to the board revision, firmware version,
commit, build options, kernel SHA-256 and the full UART log:

- [ ] Repeated cold boots reach `V3D backend selected (clear, present)`; note
      any boot that falls back and its stage and code.
- [ ] Warm reboots after a previous V3D session bring the device up again.
- [ ] `V3D=0` image boots, logs the build-time message and renders correctly.
- [ ] Injected failures in disposable builds (wrong expected identity, a job
      list that never completes, a corrupted self-test compare, a withheld
      bridge acknowledgement) each log the expected stage, fall back to the
      CPU and keep the UI usable. A failure injected after boot keeps the
      current frame.
- [ ] Pixel comparison: capture scanout after clears, mixed CPU shapes and
      presents on the V3D image and the `V3D=0` image; they must be identical,
      including colour order and the alpha byte.
- [ ] Sustained mixed CPU/GPU rendering with touch input shows no stale or
      torn regions (CPU cache and device cache transitions).
- [ ] Frame times for clear and present on both images, recorded before any
      performance statement.
- [ ] After a runtime failure, the freed 6 MiB is reused without corruption.

### Open hardware questions

These could not be confirmed from the sources and are handled conservatively
(a failing check selects the CPU renderer):

- Whether the firmware accepts `SET_CLOCK_STATE` for the V3D clock and how it
  reports the V3D domain state on current firmware.
- Whether the first bridge stop on a cold domain is acknowledged, and whether
  cycling `PM_V3DRSTN` conflicts with the firmware's own domain management.
- Whether V3D can store into the firmware-allocated framebuffer. The boot
  self-test reads the page back; a mismatch disables the whole backend.
- Whether stores clip to a frame that is not a multiple of the 64-pixel tile
  size. The guard page after the surface is checked by the self-test.
- Whether masked interrupts still latch in the status registers. Completion
  does not depend on them and MMU faults are also read from `MMU_CTL`.
- Which `V3D_ERR_STAT` bits are status rather than errors; the register is
  only logged.
- Real cache behaviour of the CPU/GPU hand-over and the cost of the per-job
  TLB flushes.

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
cmake -S . -B /path/to/driver-tests
cmake --build /path/to/driver-tests
ctest --test-dir /path/to/driver-tests -L drivers --output-on-failure
python -m unittest discover -s tests/tools -v
python tests/arm/pi4_drivers.py /path/to/fresh/kernel8.elf -v
python tests/arm/v3d_boot.py /path/to/default/kernel8.elf /path/to/v3d0/kernel8.elf -v
```

The native models decode GPU command packets and model SDHCI, backplane RAM,
firmware responses and wireless events. Tests use synthetic data, not vendor
firmware. They cover successful paths, bounds, ownership, counter/timer wrap,
timeouts, malformed frames, authentication rejection and receive overflow.
They do not model GPU microarchitecture, RF, firmware internals or physical DMA.

The V3D model (`tests/support/v3d_model.c`) sits behind `HwOps`. It models
the firmware properties, reset line, bridges, identity and MMU registers, and
executes the generated control lists tile by tile with frame clipping. The
device reads a RAM shadow that changes only when the driver cleans the CPU
cache, the CPU sees device output only after it invalidates, and device
addresses go through the TLB snapshot of the last flush, so a missing cache
or TLB step shows up as wrong pixels or a failed check. A job stays in flight
for several polls; touching its buffers, the cache or other registers in that
window fails the test. `v3d_commands_and_mmu` covers successful bring-up,
failure at every bring-up stage, job timeout and fault, quarantine and the
exact mapped page set. `render_backends` runs the production renderer, adapter
and engine against CPU rendering after every operation, including failures
injected mid-session. `tests/arm/v3d_boot.py` boots the linked default and
`V3D=0` images through the real boot coordinator with MMIO-level models of a
working, absent and failing device and checks the selected backend, the
presented frame and the memory returned to the allocator.

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
- [Linux v6.12 BCM2835 power domains](https://github.com/torvalds/linux/blob/v6.12/drivers/pmdomain/bcm/bcm2835-power.c): V3D reset line and AXI bridge stop/start order, GPL-2.0-or-later portions used under GPL version 3.
- [Mesa 24.0 Broadcom packet definitions](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-24.0.0/src/broadcom/cle/v3d_packet.xml): V3D 4.2 control-list field layouts used to check the generated packets and the test decoders; [MIT notice](licenses/mesa-broadcom-cle-MIT.txt).
- [Firmware mailbox property interface](https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface) and Linux v6.12 `include/soc/bcm2835/raspberrypi-firmware.h`: power-domain and clock property tags.
- [Linux v6.12 brcmfmac](https://github.com/torvalds/linux/tree/v6.12/drivers/net/wireless/broadcom/brcm80211/brcmfmac): CYW43455, SDPCM/BCDC and WPA firmware protocol; [ISC notice](licenses/brcmfmac-ISC.txt).
- [BCM2711 device tree](https://github.com/torvalds/linux/blob/v6.12/arch/arm/boot/dts/broadcom/bcm2711.dtsi) and [BCM2711 peripherals](https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf).
