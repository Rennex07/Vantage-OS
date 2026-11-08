# SD storage and timer implementation

## Scope and status

The C driver is an SD command engine and Pi 4 EMMC2 adapter. It
supports SD v2 cards that answer CMD8: SDSC with byte addressing, and SDHC/SDXC
with 512-byte sector addressing. It uses a one-bit bus and default-speed clocks
up to 25 MHz. Reads use CMD17; writes use CMD24 followed by transfer completion,
DAT idle and CMD13 readiness/status checks. Erase uses CMD32/33/38 with an exact,
erase-unit-aligned range. There are no DMA buffers, allocation or interrupts.

**Normal boot does not initialize SD.** Native and linked ARM regression tests
have been added, but physical Pi validation is pending. An explicit
`STORAGE_VALIDATION=1` image mounts a provisioned data partition and writes/checks
a reboot probe. See [persistent storage](STORAGE.md) for provisioning, format,
build flags and the hardware acceptance checklist. The simulator can mount a
regular disk image through `VANTAGE_STORAGE_IMAGE`.

The previous tutorial-derived SD implementation and timer helpers were replaced,
not renamed or reformatted. The new engine uses explicit controller callbacks,
per-card state, checked capacities and a different single-sector transfer path.
The removed implementation and its notice remain in Git history. The current
driver follows the project's GPL-3.0 license. Other project components and
third-party assets retain their applicable licenses.

## Layout and interfaces

- `drivers/sd.c`: portable command sequencing, clock division, CSD decoding,
  timeouts and PIO read/write/erase. `SdCard` is caller-owned; `SdIo` supplies 32-bit register
  access and microsecond timing. All callbacks must return promptly.
- `drivers/sd_pi4.c`: fixed EMMC2 MMIO adapter at ARM physical address
  `0xfe340000`. Calls are restricted to masked EL1 on core 0, from boot or
  immutable core-0-pinned tasks. Movable tasks are rejected even when temporarily
  running there. MMIO accesses include ordering barriers.
- `kernel/sched/delays/delay.c`: the BCM2711 1 MHz system timer, sampled
  high/low/high with a retry on carry. Microsecond and millisecond delays use
  unsigned 64-bit elapsed time; millisecond multiplication is widened first.

`sd_card_init(card, io, base_clock_hz)` initializes a zeroed or previously
initialized card. `io` must not alias the card's storage. The instance/controller
must be exclusively owned throughout each call; no concurrent or reentrant use
is supported. The callbacks/context must remain valid during calls.

`sd_card_read(card, lba, count, destination, destination_bytes)` returns `SD_OK`
only after every sector's transfer-complete indication. It accepts 1–128 sectors
and unaligned destinations. It checks the supplied buffer size, pointer overflow
and the full sector range against CSD capacity before I/O. The destination must
be writable kernel/caller storage and must not overlap the card state. This is
not a user-buffer validation API.

`sd_card_write` has the same sector/count/buffer-size contract, accepts unaligned
sources and checks the CSD write-protection bits and write command class. Errors
can mean some sectors changed: do not treat a failed request as atomic.
`sd_card_sync` checks READY_FOR_DATA and the TRAN state, including deferred R1
errors. No optional volatile card write cache is enabled; this is not a generic
cache-flush implementation. Completion still requires physical power-loss tests.

`sd_card_erase` accepts 1–65,536 sectors within capacity and aligned to
`erase_sectors`, decoded from ERASE_BLK_EN, SECTOR_SIZE and WRITE_BL_LEN. It
rejects unsupported command classes and permanent/temporary write protection.
Erased byte values are unspecified. It is destructive and is never called by
filesystem commits or normal boot. Raw driver callers must enforce their own
partition boundaries. Software cannot promise secure deletion inside card flash.

`sd_init(base_clock_hz)`, `sd_read_blocks`, `sd_write_blocks`, `sd_sync`,
`sd_erase_blocks` and `sd_sector_count` expose the singleton Pi adapter.
The legacy declared spelling `sd_readBlock(lba, buffer, count)` remains as a
byte-count/zero-on-error wrapper. It cannot verify the caller's actual allocation;
new callers should use the size-checked interface. The old private `sd_cmd` and
global `sd_err` interface is removed. `kernel/fs/storage_pi4.c` adapts the checked
interfaces to the filesystem's sector device. `sd_init` requires the actual
base-clock frequency.

Invalid requests do no controller I/O. An unsupported host version is queried
without reconfiguring the controller. Once setup starts, hardware or protocol errors mark the card
unready, disable interrupt signaling and attempt a bounded full host reset. A
successful reset stops its output clock; a wedged host that cannot reset remains
unusable. Clock gating is not used to abort an inhibited transaction. Reinitialization
is required before further I/O. Error returns may leave partial data in the
destination: discard the entire request's result. Do not use partial bytes as
filesystem metadata. `last_interrupt` and `last_response` retain diagnostics.

## Protocol and timing

Initialization resets the host, selects a divided clock at or below 400 kHz,
provides initial card clocks, then sends CMD0, CMD8, CMD55/ACMD41, CMD2, CMD3,
CMD9 and CMD7. SDSC additionally uses CMD16 to select 512-byte blocks. CMD8 echo,
APP_CMD acceptance, OCR voltage/capacity, R6 address/errors and CSD structure are
validated. CSD v1 and v2 capacity fields are decoded from the SDHCI R2 register
layout; capacities up to 2^32 sectors use a 64-bit counter. SD v1, MMC, SDIO,
SDUC and 1.8V/UHS operation are outside this implementation's scope.

The host must support SDHCI v3's ten-bit divided-clock mode with v4 mode off.
Clock divisors round upward so the SD clock never exceeds its target. The source
clock is explicit and must remain stable during use. There is no hard-coded
assumed source frequency or automatic firmware mailbox request.

Status bits use write-one-to-clear semantics. A controller error wins over a
simultaneous completion bit. R1 errors are separate from host errors. Busy
responses and data operations require transfer completion, not just command
completion or buffer-ready. PIO words are packed/unpacked without aligned casts.

Host reset, inhibit and command polls have a 100 ms deadline; clock stabilization
has a 150 ms deadline; reads and selection-busy waits have a 1 s deadline.
Write completion and CMD13 readiness use 5 s bounds; erase-busy polls use a 120 s
bound. The host hardware timeout can report failure earlier. Each register poll
also has a 100,000-iteration cap; CMD13 has a 5,000-command cap.
Card power-up negotiation has a 1 s elapsed-time check and at most 1,000 attempts;
individual command waits may extend the total time. These bounds assume the
callbacks return. The hardware delay implementation requires a running system
timer. `wait_cycles` is an instruction-iteration helper, not calibrated timing.
Both `wait_msec` and `wait_msec_st` now mean actual milliseconds.

## Pi 4 handoff and validation

Before calling `sd_init` in a disposable hardware-validation build:

1. Confirm a Pi 4 Model B and the firmware/device-tree EMMC2 mapping. The Pi 4
   SD slot is connected to EMMC2; it is not the legacy GPIO 48–53 SD pin group.
   Those GPIOs participate in Ethernet on Pi 4. This adapter makes no GPIO changes.
2. Establish/confirm SD-slot power and **3.3V signaling** through board/firmware
   setup. A firmware boot may have used another signaling mode; this driver does
   not control the external power/voltage GPIO expander or perform a voltage
   switch. A warm handoff without this confirmation is not supported.
3. Obtain and hold the actual EMMC2 source clock using firmware/board clock
   configuration. Pass its frequency in hertz, not the desired SD bus frequency.
   Do not assume the old tutorial's clock constant applies.
4. Call from core 0, EL1, with exceptions masked and exclusive controller access.
   The current identity mappings must cover the controller and system timer as
   Device memory. Use a kernel-owned destination buffer.
5. Use a spare card with known raw-sector contents. Compare reads of sector 0,
   adjacent sectors, and the last valid sector with a trusted host-side image;
   exercise aligned and unaligned destinations. Confirm CSD-reported capacity.
6. Repeat cold boots across several cards. Test absent/unresponsive cards and
   bounded failures. Confirm errors require successful reinitialization before
   reuse. Record board/firmware, source clock, commit, image hash and results.

Keep SD initialization disabled in normal boot until physical handoff,
read/write/erase and reboot-persistence testing is accepted. Filesystem/partition
handling and the corresponding validation procedure are in [STORAGE.md](STORAGE.md).

## Regression tests

Build the native tests with `cmake -S drivers -B <build>` and
`cmake --build <build> --config Release`, then run
`ctest --test-dir <build> -C Release --output-on-failure`. The simulator build also
includes `sd_controller` and `delays`. The controller model independently handles
write-one-to-clear status, command response types, card replies, the read FIFO
and transfer completion. Cases include writes, CMD13 programming/errors, erase
endpoints, unit alignment, write protection, CMD8 mismatches, both CSD formats,
maximum capacity, byte/sector addressing, unaligned buffers with guards,
argument rejection without I/O, CRC/timeout failures, stuck hardware, frozen
clock callbacks, counter wrap and explicit reinitialization. Timer tests cover
carry retry, delay units, zero delays, 64-bit wrap and large millisecond values.

With the existing Unicorn/Capstone dependencies and a fresh AArch64 kernel:

```sh
python tests/arm/storage.py /path/to/kernel8.elf -v
```

This runs the linked Pi adapter, command engine and timer code against modeled
MMIO. It checks ownership guards, sector bytes/buffer guards, error propagation,
reinitialization and timer carry/units. It does not validate electrical behavior,
firmware power/clock state, physical cache ordering or actual card compatibility.

## References used

- SD Association, **SD Host Controller Simplified Specification v4.20**:
  register map and response layout (2.2.5), transfer/command fields, clock
  division (2.2.14), reset/status and PIO transfer rules.
- SD Association, **Physical Layer Simplified Specification v6.00**:
  initialization and command responses (4.2, 4.7–4.10), card status,
  CSD v1/v2 fields and capacity formulas (5.3).
  Official specification index: <https://www.sdcard.org/downloads/pls/>.
- Raspberry Pi's published BCM2711 device-tree descriptions for
  [EMMC2's register mapping](https://github.com/raspberrypi/linux/blob/rpi-6.12.y/arch/arm/boot/dts/broadcom/bcm2711.dtsi)
  and [Pi 4 slot wiring, supplies and GPIO functions](https://github.com/raspberrypi/linux/blob/rpi-6.12.y/arch/arm/boot/dts/broadcom/bcm2711-rpi-4-b.dts).
- Broadcom/Raspberry Pi, BCM2711 ARM Peripherals, System Timer register layout:
  <https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf>.

These are hardware/protocol references; third-party driver implementation text
was not used as a source for the replacement.
