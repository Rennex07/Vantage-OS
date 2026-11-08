"""Create/provision regular disk-image files for the experimental storage format.

Never opens a physical disk. Formatting an existing image is explicit and only
changes its selected type-0xda primary partition's two snapshot banks.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import zlib

SECTOR = 512
FILES = 16
CAPACITY = 4096
PAYLOAD = FILES * 9
BANK = PAYLOAD + 1
MIN_SECTORS = BANK * 2
MAGIC = b'VSTORE1\0'


def empty_bank(bank, generation, first, sectors):
    payload = bytes(PAYLOAD * SECTOR)
    header = bytearray(SECTOR)
    struct.pack_into('<8sIIQIIQQ', header, 0, MAGIC, 1, bank, generation,
                     zlib.crc32(payload), PAYLOAD, first, sectors)
    struct.pack_into('<I', header, 508, zlib.crc32(header[:508]))
    return bytes(header) + payload


def new_image():
    first, sectors = 2048, MIN_SECTORS
    image = bytearray((first + sectors) * SECTOR)
    image[510:512] = b'\x55\xaa'
    image[450] = 0xda
    struct.pack_into('<II', image, 454, first, sectors)
    for bank in range(2):
        start = (first + bank * BANK) * SECTOR
        image[start:start + BANK * SECTOR] = empty_bank(bank, bank + 1, first, sectors)
    return image


def partition(mbr, total_sectors, index):
    if index not in range(4) or len(mbr) != SECTOR or mbr[510:] != b'\x55\xaa':
        raise ValueError('Invalid primary MBR partition selection')
    entries = []
    for i in range(4):
        boot, kind = mbr[446 + 16 * i], mbr[450 + 16 * i]
        first, count = struct.unpack_from('<II', mbr, 454 + 16 * i)
        if not (boot or kind or first or count):
            entries.append((0, 0, 0))
            continue
        if (boot not in (0, 0x80) or kind in (0, 5, 0x0f, 0x85, 0xee) or
                not first or not count or first >= total_sectors or count > total_sectors - first):
            raise ValueError('Invalid, extended, or GPT partition entry')
        if any(old_count and first < old_first + old_count and old_first < first + count
               for _, old_first, old_count in entries):
            raise ValueError('Overlapping primary partitions')
        entries.append((kind, first, count))
    kind, first, count = entries[index]
    if kind != 0xda or count < MIN_SECTORS:
        raise ValueError('Select a type-0xda data partition of at least 290 sectors')
    return first, count


def inspect_bank(data, bank, first, sectors):
    header, payload = data[:SECTOR], data[SECTOR:]
    if len(data) != BANK * SECTOR:
        return {'bank': bank, 'valid': False}
    fields = struct.unpack_from('<8sIIQIIQQ', header)
    magic, version, identity, generation, crc, count, start, length = fields
    valid = (magic == MAGIC and version == 1 and identity == bank and generation > 0 and
             count == PAYLOAD and start == first and length == sectors and
             struct.unpack_from('<I', header, 508)[0] == zlib.crc32(header[:508]) and
             crc == zlib.crc32(payload) and not any(header[48:508]))
    files = []
    names = set()
    for i in range(FILES):
        record = payload[i * 9 * SECTOR:(i + 1) * 9 * SECTOR]
        name = record[:32].split(b'\0', 1)[0]
        used, size = struct.unpack_from('<II', record, 32)
        allowed = b'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-'
        valid &= (used in (0, 1) and size <= CAPACITY and not any(record[40:SECTOR]) and
                  not any(record[len(name):32]) and not any(record[SECTOR + size:]))
        if used:
            valid &= (0 < len(name) < 32 and name not in (b'.', b'..') and
                      all(c in allowed for c in name) and name not in names)
            names.add(name)
            files.append({'path': '/ram/' + name.decode('ascii', errors='replace'), 'size': size})
        else:
            valid &= not name and not size
    return {'bank': bank, 'valid': bool(valid), 'generation': generation, 'files': files if valid else []}


def regular_file(path, mode):
    f = open(path, mode)
    if not stat.S_ISREG(os.fstat(f.fileno()).st_mode):
        f.close()
        raise ValueError('Only regular image files are supported')
    return f


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('create', 'format', 'inspect'))
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--partition', type=int, default=0)
    parser.add_argument('--confirm-format', action='store_true')
    args = parser.parse_args()
    if args.command == 'create':
        if args.partition != 0:
            parser.error('create makes partition 0')
        with regular_file(args.image, 'xb') as f:
            f.write(new_image()); f.flush(); os.fsync(f.fileno())
    else:
        if args.command == 'format' and not args.confirm_format:
            parser.error('format requires --confirm-format and destroys the selected volume')
        with regular_file(args.image, 'r+b' if args.command == 'format' else 'rb') as f:
            size = os.fstat(f.fileno()).st_size
            if size % SECTOR:
                raise ValueError('Image size is not a multiple of 512 bytes')
            first, count = partition(f.read(SECTOR), size // SECTOR, args.partition)
            if args.command == 'format':
                # Invalidate both old descriptors durably before provisioning.
                for bank in range(2):
                    f.seek((first + bank * BANK) * SECTOR); f.write(bytes(SECTOR))
                f.flush(); os.fsync(f.fileno())
                for bank in range(2):
                    f.seek((first + bank * BANK) * SECTOR)
                    f.write(empty_bank(bank, bank + 1, first, count))
                f.flush(); os.fsync(f.fileno())
            else:
                banks = []
                for bank in range(2):
                    f.seek((first + bank * BANK) * SECTOR)
                    banks.append(inspect_bank(f.read(BANK * SECTOR), bank, first, count))
                valid = [b for b in banks if b['valid']]
                if len(valid) == 2 and valid[0]['generation'] == valid[1]['generation']:
                    raise ValueError('Ambiguous equal snapshot generations')
                print(json.dumps({'partition': args.partition, 'first_sector': first, 'sectors': count,
                                  'recovered': len(valid) == 1, 'banks': banks}, indent=2))
                if not valid:
                    raise ValueError('No valid storage snapshot')
    # Bounded-memory image hash for validation records, never device identifiers.
    with regular_file(args.image, 'rb') as f:
        digest = hashlib.file_digest(f, 'sha256').hexdigest()
    print('Image SHA-256:', digest)


if __name__ == '__main__':
    main()
