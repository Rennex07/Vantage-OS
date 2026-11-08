# Persistent SD files — S1 software implementation

**Physical Raspberry Pi validation is pending. S1 is not hardware-accepted.**
Normal boot leaves EMMC2 untouched and file syscalls return `FILE_UNAVAILABLE`.
Only an explicit hardware-validation build mounts SD. Blank/corrupt cards are
never automatically formatted, and failures never fall back to volatile files.

## Scope and interface

The portable SD engine in `drivers/sd.c` is the retained implementation; the
nightly tutorial driver and FAT reader are not combined with it. The EMMC2
adapter, read/write/erase protocol and firmware handoff are in [SD.md](SD.md).

`kernel/fs/storage.c` implements a bounded, checksummed snapshot filesystem over
512-byte sectors. `kernel/fs/files.c` uses it behind the existing `SYS_FILE_*`
IDs, payload layouts, task-owned handles and file operations. The `/ram/<name>`
spelling is retained for compatibility; it now names persistent files. There is
no separate RAM-file fallback. The independent legacy block RAM disk is unused.

Version 1 retains 16 files, 4096 bytes per file, 16 handles, 31-byte ASCII names
and 256 bytes per I/O syscall. It is not FAT and cannot hold full radio firmware
images at these limits. Larger assets require a separately reviewed storage
format/capacity extension. Files share a namespace, not user-account permissions.
There is no executable loading in this change.

Creation, truncation, each nonempty write and removal commit synchronously before
returning success. Close/reap only release handles; there is no new flush syscall.
Existing errors retain their values; `FILE_CORRUPT=-8` and `FILE_IO_ERROR=-9` are
additive. Invalid arguments/capacity failures do not change files. An I/O failure
quarantines the mount, so staged/uncertain data cannot subsequently be reported
as durable. Close or task exit can still release handles; unmount/remount is
required for recovery. A failed commit may nevertheless be present after reboot.

All kernel APIs are non-reentrant, non-yielding, core-0 operations with trusted
kernel buffers. Mount/unmount reject live handles; handle generations are not
reset by remount. A mounted RAM cache serves reads; media checksums are verified
at mount and commit readback, not continuously on every cached read.

## Partition and on-disk format

Select an explicit primary MBR index 0–3, of type `0xda`, at least **290 sectors**
(148,480 bytes). Every nonempty primary entry is capacity-checked and overlaps
are rejected. Extended partitions and protective GPT MBRs are rejected. Sector 0,
the firmware FAT boot partition and other partitions are outside volume writes.
The data partition must be created separately in a trusted partitioning tool.

Each of two banks is 145 sectors: a descriptor and 144 payload sectors. Each file
occupies one metadata sector plus eight data sectors. All integers are explicitly
little-endian; C structs are not written to disk. Unused bytes are zeroed.

Descriptor offsets:

| Offset | Field |
|---|---|
| 0 | Eight bytes `VSTORE1\0` |
| 8 / 12 | 32-bit format version 1 / bank index |
| 16 | Nonzero 64-bit generation |
| 24 / 28 | CRC-32 of payload / payload sector count 144 |
| 32 / 40 | 64-bit partition start / sector count |
| 48–507 | Zero |
| 508 | CRC-32 of descriptor bytes 0–507 |

File metadata holds name bytes 0–31, 32-bit used flag at 32, byte size at 36,
and zero padding. Names, sizes, used flags, duplicate names, padding and bytes
beyond EOF are checked before exposure. CRC uses the standard reflected CRC-32
polynomial `0xedb88320`, with initial/final inversion. It detects accidental
corruption, not malicious tampering; this is not an image-signature mechanism.

Commit order:

1. Invalidate the inactive bank's descriptor, then synchronize.
2. Write its complete payload, synchronize, and verify payload readback CRC.
3. Publish the new checksummed descriptor, synchronize, and verify readback.
4. Update the in-memory active bank and generation. Generation never wraps.

Mount selects the highest valid generation. Equal valid generations are rejected.
One invalid bank yields `STORE_MOUNT_RECOVERED`, with a boot/simulator warning;
both invalid banks yield corruption, not an empty filesystem. Read failures are
I/O errors rather than silently selecting stale data. Recovery can discard a
newer snapshot if that snapshot is damaged, and the warning must be investigated.

This first implementation rewrites 74,752 bytes per mutation. It favors a small,
auditable recovery protocol over write amplification or throughput. No speed or
flash-endurance claim is made. Recovery assumes completed syncs are durable and
failed writes affect only their addressed sectors. SD-card FTL/controller behavior
can violate those assumptions under power loss; physical testing is mandatory.

## Provision a regular disk image

Python 3.11+ is required for image tooling. Commands only accept regular files,
never a physical disk. `create` refuses to overwrite an existing path:

```sh
python tools/storage_image.py create --image /path/to/test-data.img
python tools/storage_image.py inspect --image /path/to/test-data.img
```

The new image has one type-0xda partition starting at sector 2048. It is a data
test image, **not a bootable Pi card image**. To provision a copy of a complete
bootable card image with a separately created type-0xda partition:

```sh
python tools/storage_image.py format --image /path/to/card-copy.img --partition 1 --confirm-format
python tools/storage_image.py inspect --image /path/to/card-copy.img --partition 1
```

Formatting destroys that volume. Keep a backup and verify the selected partition
before transferring an image to a spare card using a trusted host tool. The tool
prints an image SHA-256 for validation records. It does not repartition images.

The simulator mounts partition 0 of an existing image when
`VANTAGE_STORAGE_IMAGE` is set to its path. Its regular-file adapter uses
`fflush` and `_commit`/`fsync`; it never formats a missing or invalid volume.
The terminal provides file commands and a touch-first text editor; see
[TERMINAL.md](TERMINAL.md). Fresh-process round-trip tests exercise persistence.

## Hardware-validation image and acceptance

First establish the Pi 4 EMMC2 power, 3.3V signaling and stable source clock as
described in [SD.md](SD.md). The clock below is illustrative, not a detected or
recommended value:

```sh
make BUILD_DIR=/path/to/validation STORAGE_VALIDATION=1 SD_BASE_CLOCK_HZ=100000000 STORAGE_PARTITION=1
```

The build requires Python 3 as well as the GNU AArch64 toolchain. Build option
changes invalidate objects, including when reusing a directory. Normal builds
default to `STORAGE_VALIDATION=0`. The validation path mounts an already formatted
volume, halts on failure, and uses the reserved `/ram/.s1-reboot` test file:

- First run creates a deterministic 256-byte probe and logs that it was written.
- A later run verifies its exact contents/length and logs a match. A matching
  probe alone is not proof of reboot; retain distinct cold-boot logs.
- An empty probe is retried as interrupted initialization: creation and its first
  write are separate snapshot commits. This logs a write, not a persistence match.
- A nonempty damaged probe halts; the validation image does not erase or reformat it.

Before enabling normal storage boot, physically verify all of the following on
spare cards. Keep each result tied to an exact image; no model result substitutes
for these checks:

- [ ] Record Pi model/revision, boot firmware version/configuration, card model,
      source clock, commit, build flags, kernel image SHA-256 and complete UART log.
- [ ] Compare sector 0, adjacent sectors, last valid sector and aligned/unaligned
      reads against a trusted host image; confirm CSD capacity and failure bounds.
- [ ] In a disposable kernel harness, call `sd_write_blocks` on a known test-only
      range inside the data partition. Read it back, power-cycle, and verify from
      both the Pi and a trusted host. Check unchanged guard sectors outside it.
- [ ] In a separate disposable harness, use `sd_erase_blocks` only on a deliberately
      reserved range after both snapshot banks, with CSD erase-unit-aligned start
      and count. Verify neighboring sectors are unchanged. Do not assume zero or
      `0xff` erased bytes. Never erase either snapshot bank or a boot partition.
- [ ] Preserve first-run probe output; cold-power-cycle the same image and record
      a successful probe match. Exercise file create, write, seek, list, truncate,
      remove, task-exit cleanup and subsequent cold-boot reads through syscalls.
- [ ] Interrupt power at different commit phases. Verify old/new complete files,
      explicit recovery warnings, and unchanged boot/neighbor partitions.
- [ ] Deliberately damage metadata and payload on copied images. One valid bank
      must warn/recover; two invalid banks must halt mounting without formatting.
- [ ] Exercise absent/unresponsive and write-protected media, repeated cold boots,
      multiple cards, CRC/timeout failures, and explicit reinitialization/remount.

Raw erase testing needs a larger partition than the 290-sector minimum, with
room for at least one aligned erase unit beyond the banks. Remove disposable
test harness changes before shipping an image. Record acceptance in a separate
reviewed change before modifying the normal-boot gate.

## Software verification

```sh
cmake -S kernel/fs -B /path/to/files-tests
cmake --build /path/to/files-tests --config Release
ctest --test-dir /path/to/files-tests -C Release --output-on-failure
cmake -S drivers -B /path/to/driver-tests
cmake --build /path/to/driver-tests --config Release
ctest --test-dir /path/to/driver-tests -C Release --output-on-failure
python -B -m unittest discover -s tests/tools -v
python -B tests/arm/files.py /path/to/fresh/kernel8.elf -v
python -B tests/arm/storage.py /path/to/fresh/kernel8.elf -v
```

Native tests cover persistent syscall semantics, process-exit/remount persistence,
partition bounds, semantic corruption and commit failures at 294 I/O boundaries,
with torn lengths 0/1/255/511/512 on immediate and sync-buffered devices. Linked
tests execute actual ARM file/syscall/SD-adapter code against modeled sectors and
MMIO, including a mounted filesystem round trip. CI additionally runs host
address/undefined-behavior sanitizers. These checks do not validate electrical
behavior, actual cards, physical power loss or firmware handoff.
