"""Exercise linked SD/timer code and the Pi 4 adapter with modeled registers.

Usage: python tests/arm/storage.py path/to/kernel8.elf -v
Requires the dependencies used by memory_management.py; not physical Pi testing.
"""
import struct
import sys
import unittest

from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE

from memory_management import Image, Machine
from file_storage import new_image

EMMC2 = 0xfe340000
TIMER = 0xfe003000
BUFFER = 0x5000001


def signed(value):
    value &= 0xffffffff
    return value - (1 << 32) if value & (1 << 31) else value


class StorageMachine(Machine):
    def __init__(self, image):
        super().__init__(image)
        self.uc.mem_map(EMMC2, 4096)
        self.uc.mem_map(TIMER, 4096)
        self.registers = {0xfc: 0x20000}
        self.commands, self.accesses = [], 0
        self.time, self.word, self.lba = 0, 0, 0
        self.echo, self.fail_read = 0x1aa, False
        self.fail_write = False
        self.media, self.write_mode = {}, False
        self.erase_first, self.erase_last = 0, 0
        self.uc.hook_add(UC_HOOK_MEM_READ, self.read_host, begin=EMMC2, end=EMMC2 + 255)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.write_host, begin=EMMC2, end=EMMC2 + 255)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.read_timer, begin=TIMER, end=TIMER + 15)

    def read_timer(self, uc, access, address, size, value, _):
        assert size == 4
        if address == TIMER + 4:
            value = self.time & 0xffffffff
            self.time = (self.time + 1000) & ((1 << 64) - 1)
        else:
            assert address == TIMER + 8
            value = self.time >> 32
        uc.mem_write(address, struct.pack('<I', value))

    def read_host(self, uc, access, address, size, value, _):
        assert size == 4
        self.accesses += 1
        offset = address - EMMC2
        if offset == 0x20:
            assert self.word < 128
            sector = self.media.get(self.lba)
            if sector is None:
                sector = bytes((self.lba + i) & 255 for i in range(512))
            value = int.from_bytes(sector[self.word * 4:self.word * 4 + 4], 'little')
            self.word += 1
            if self.word == 128:
                self.registers[0x30] |= 2
                self.registers[0x24] = 0
        else:
            value = self.registers.get(offset, 0)
        uc.mem_write(address, struct.pack('<I', value))

    def write_host(self, uc, access, address, size, value, _):
        assert size == 4
        self.accesses += 1
        r = self.registers
        offset = address - EMMC2
        if offset == 0x20:
            assert self.write_mode and self.word < 128
            sector = self.media.setdefault(self.lba, bytearray(512))
            sector[self.word * 4:self.word * 4 + 4] = value.to_bytes(4, 'little')
            self.word += 1
            if self.word == 128:
                r[0x30] |= 2
                r[0x24] = 0
            return
        if offset == 0x30:
            r[offset] = r.get(offset, 0) & ~value
        elif offset == 0x2c:
            r[offset] = 0 if value & 0x01000000 else value | (2 if value & 1 else 0)
            if value & 0x01000000:
                r[0x24] = r[0x30] = 0
        else:
            r[offset] = value
        if offset != 0x0c:
            return
        index, argument = value >> 24, r[0x08]
        assert r[0x2c] & 4 and not r[0x38]
        self.commands.append((index, argument))
        r[0x30] |= 1
        r[0x10] = 0
        if index == 8:
            r[0x10] = self.echo
        elif index == 55:
            r[0x10] = 0x20
        elif index == 41:
            assert argument == 0x40ff8000
            r[0x10] = 0xc0ff8000
        elif index == 3:
            r[0x10] = 0x12340000
        elif index == 9:
            r[0x10], r[0x14], r[0x18], r[0x1c] = 9 << 14, 0x1fff40, 0x30900, 0x400000
        elif index == 7:
            r[0x30] |= 2
        elif index == 17:
            assert r[0x04] == 0x10200 and (value & 0xffffff) == 0x3a0010
            self.word, self.lba = 0, argument
            r[0x24] = 2
            r[0x30] |= 0x108000 if self.fail_read else 0x20
            self.write_mode = False
        elif index == 24:
            assert r[0x04] == 0x10200 and (value & 0xffffff) == 0x3a0000
            self.word, self.lba, self.write_mode = 0, argument, True
            r[0x24] = 2
            r[0x30] |= 0x108000 if self.fail_write else 16
        elif index == 13:
            assert argument == 0x12340000
            r[0x10] = 0x900
        elif index == 32:
            self.erase_first = argument
        elif index == 33:
            self.erase_last = argument
        elif index == 38:
            assert argument == 0
            for lba in range(self.erase_first, self.erase_last + 1):
                self.media[lba] = bytearray(b'\xff' * 512)
            r[0x30] |= 2
        else:
            assert index in (0, 2)


class StorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = Image(sys.argv[1])

    def test_pi_adapter_initializes_and_reads_unaligned_buffer(self):
        m = StorageMachine(self.image)
        self.assertEqual(m.call('sd_init', 100000000), 0)
        self.assertEqual([c for c, _ in m.commands], [0, 8, 55, 41, 2, 3, 9, 7])
        m.uc.mem_write(BUFFER - 1, b'\xa7' * 1026)
        self.assertEqual(m.call('sd_read_blocks', 17, 2, BUFFER, 1024), 0)
        expected = bytes((17 + block + i) & 255 for block in range(2) for i in range(512))
        self.assertEqual(m.uc.mem_read(BUFFER, 1024), expected)
        self.assertEqual(m.uc.mem_read(BUFFER - 1, 1), b'\xa7')
        self.assertEqual(m.uc.mem_read(BUFFER + 1024, 1), b'\xa7')
        self.assertEqual(m.call('sd_readBlock', 1, BUFFER, 1), 512)
        self.assertGreaterEqual(m.minimum_sp, self.image.symbols['__kernel_stack_bottom'] + 64)

    def test_owner_and_buffer_rejections_do_not_touch_controller(self):
        m = StorageMachine(self.image)
        for core, level, masks in ((1, 4, 0x3c0), (0, 8, 0x3c0), (0, 4, 0)):
            m.core, m.level, m.daif = core, level, masks
            self.assertEqual(signed(m.call('sd_init', 100000000)), -3)
            self.assertEqual(signed(m.call('sd_read_blocks', 0, 1, BUFFER, 512)), -3)
            self.assertEqual(signed(m.call('sd_write_blocks', 0, 1, BUFFER, 512)), -3)
            self.assertEqual(signed(m.call('sd_erase_blocks', 0, 1)), -3)
            self.assertEqual(m.accesses, 0)
        m.core, m.level, m.daif = 0, 4, 0x3c0
        self.assertEqual(m.call('sd_init', 100000000), 0)
        accesses = m.accesses
        self.assertEqual(signed(m.call('sd_read_blocks', 0, 1, BUFFER, 511)), -3)
        self.assertEqual(m.call('sd_readBlock', 0, BUFFER, 0), 0)
        self.assertEqual(m.accesses, accesses)

    def test_linked_unaligned_writes_status_erase_and_fail_closed(self):
        m = StorageMachine(self.image)
        self.assertEqual(m.call('sd_init', 100000000), 0)
        expected = bytes(i & 255 for i in range(1024))
        m.uc.mem_write(BUFFER, expected)
        self.assertEqual(m.call('sd_write_blocks', 5, 2, BUFFER, 1024), 0)
        self.assertEqual(m.call('sd_read_blocks', 5, 2, BUFFER, 1024), 0)
        self.assertEqual(m.uc.mem_read(BUFFER, 1024), expected)
        self.assertEqual(m.call('sd_erase_blocks', 5, 2), 0)
        self.assertEqual((m.erase_first, m.erase_last), (5, 6))
        self.assertEqual(m.media[5], b'\xff' * 512)
        m.fail_write = True
        self.assertEqual(signed(m.call('sd_write_blocks', 5, 1, BUFFER, 512)), -1)
        self.assertEqual(signed(m.call('sd_sync')), -5)

    def test_pi_adapter_mounts_and_persists_through_sector_syscalls(self):
        m = StorageMachine(self.image)
        disk = new_image()
        m.media = {i // 512: bytearray(disk[i:i + 512]) for i in range(0, len(disk), 512)}
        self.assertEqual(m.call('storage_start', 100000000, 0), 0)
        self.assertEqual(m.call('storage_reboot_probe'), 0)
        m.uc.mem_write(BUFFER, b'/ram/pi-test\0')
        h = m.call('files_open', 1, BUFFER, 7)
        self.assertGreater(h, 0)
        m.uc.mem_write(BUFFER + 128, b'sector-backed')
        self.assertEqual(m.call('files_write', 1, h, BUFFER + 128, 13), 13)
        self.assertEqual(m.call('files_close', 1, h), 0)
        self.assertEqual(m.call('files_unmount'), 0)
        self.assertEqual(m.call('storage_start', 100000000, 0), 0)
        self.assertEqual(m.call('storage_reboot_probe'), 1)
        h = m.call('files_open', 1, BUFFER, 1)
        self.assertEqual(m.call('files_read', 1, h, BUFFER + 256, 32), 13)
        self.assertEqual(m.uc.mem_read(BUFFER + 256, 13), b'sector-backed')
        self.assertEqual(bytes(m.media[0]), disk[:512])

    def test_echo_and_read_errors_invalidate_card(self):
        m = StorageMachine(self.image)
        m.echo = 0x1ab
        self.assertEqual(signed(m.call('sd_init', 100000000)), -4)
        self.assertFalse(m.registers[0x2c] & 4)
        m.echo = 0x1aa
        self.assertEqual(m.call('sd_init', 100000000), 0)
        m.fail_read = True
        self.assertEqual(signed(m.call('sd_read_blocks', 0, 1, BUFFER, 512)), -1)
        self.assertFalse(m.registers[0x2c] & 4)
        accesses = m.accesses
        self.assertEqual(signed(m.call('sd_read_blocks', 0, 1, BUFFER, 512)), -5)
        self.assertEqual(m.accesses, accesses)

    def test_linked_timer_reads_carry_and_waits_in_milliseconds(self):
        m = StorageMachine(self.image)
        m.time = 0x5fffffff0
        self.assertEqual(m.call('get_sys_timer'), 0x6000003d8)
        before = m.time
        m.call('wait_msec', 2)
        self.assertEqual(m.time - before, 3000)
        before = m.time
        m.call('wait_msec_st', 2)
        self.assertEqual(m.time - before, 3000)
        before = m.time
        m.call('wait_usec', 0)
        self.assertEqual(m.time, before)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    unittest.main(argv=[sys.argv[0]] + sys.argv[2:])
