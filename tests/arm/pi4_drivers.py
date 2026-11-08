"""Linked ARM V3D/ABI checks with a scripted device model, not a Pi test."""
import struct
import sys
import unittest

from unicorn import UC_HOOK_CODE
from unicorn import arm64_const as regs
from memory_management import Image, Machine, STATS

CONTEXT, OPS = 0x7000000, 0x7001000
CALLBACK = 0x7100000
TABLE, ARENA = 0x1000000, 0x1400000
GPU = 0x1000000
HUB, CORE = 0xfec00000, 0xfec04000


class GpuMachine(Machine):
    def __init__(self, image):
        super().__init__(image)
        self.clock, self.bin, self.render = 0, 255, 255
        self.owner, self.stall = True, False
        self.start, self.stop = 0, 0
        callbacks = ['read', 'write', 'time', 'owned', 'cache', 'physical', 'property']
        for i, name in enumerate(callbacks):
            address = CALLBACK + i * 4
            self.uc.hook_add(UC_HOOK_CODE, self.callback, user_data=name, begin=address, end=address)
        self.uc.mem_write(OPS, struct.pack('<8Q', 0, *[CALLBACK + i * 4 for i in range(7)]))

    def word(self, address): return struct.unpack('<I', self.uc.mem_read(address, 4))[0]

    def callback(self, uc, address, size, name):
        args = [uc.reg_read(getattr(regs, f'UC_ARM64_REG_X{i}')) for i in range(5)]
        result = 0
        if name == 'owned': result = self.owner
        elif name == 'physical': result = args[1]
        elif name == 'time': self.clock = (self.clock + 10000) & 0xffffffff; result = self.clock
        elif name == 'property':
            self.assert_property(args[1], args[2], args[3]); result = 1
        elif name == 'read':
            result = {CORE: 0x04443356, CORE + 4: 2, HUB + 0xc: 0x100, HUB + 0x10: 0x100,
                      CORE + 0x134: self.bin, CORE + 0x138: self.render}.get(args[1], 0)
        elif name == 'write':
            register, value = args[1:3]
            if register == CORE + 0x168 and not self.stall: self.bin = (self.bin + 1) & 255
            if register == CORE + 0x164: self.start = value
            if register == CORE + 0x804 and value == 2: self.stop += 1
            if register == CORE + 0x16c:
                # Independent decode of only the generated clear/copy subset.
                base = ARENA + self.start - GPU
                data = bytes(uc.mem_read(base, value - self.start))
                color = struct.unpack_from('<I', data, 11)[0]
                generic = ARENA + 0x2000
                self.assert_byte(generic, 125)
                generic += 1
                source = None
                if uc.mem_read(generic, 1) == b'\x1e':
                    source = ARENA + self.word(generic + 9) - GPU; generic += 13
                self.assert_byte(generic, 26); generic += 1
                self.assert_byte(generic, 29)
                destination = ARENA + self.word(generic + 9) - GPU
                pixels = self.word(CONTEXT + 36)
                uc.mem_write(destination, bytes(uc.mem_read(source, pixels)) if source is not None else struct.pack('<I', color) * (pixels // 4))
                self.render = (self.render + 1) & 255
        elif name == 'cache':
            assert args[1] >= TABLE and args[1] + args[2] <= ARENA + 0x400000
        uc.reg_write(regs.UC_ARM64_REG_X0, int(result))
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    def assert_byte(self, address, value): assert self.uc.mem_read(address, 1) == bytes([value])
    def assert_property(self, tag, values, count):
        assert count == 2
        if tag == 0x30002: self.uc.mem_write(values + 4, struct.pack('<I', 500000000))
        else: assert tag == 0x38030 and self.word(values) == 11


class DriverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image = Image(sys.argv[1])

    def test_linked_gpu_mmu_commands_and_counter_wrap(self):
        m = GpuMachine(self.image)
        self.assertEqual(m.call('v3d_init', CONTEXT, OPS, TABLE, ARENA, 64, 64), 1)
        self.assertEqual(m.word(TABLE + (GPU >> 12) * 4), (ARENA >> 12) | 0x30000000)
        self.assertEqual(m.word(TABLE), 0)
        self.assertEqual(m.call('v3d_self_test', CONTEXT), 1)
        self.assertEqual(m.uc.mem_read(ARENA + 0x40000, 64 * 64 * 4), b'\0' * (64 * 64 * 4))
        self.assertEqual(m.render, 2)

    def test_linked_gpu_timeout_quarantines_context(self):
        m = GpuMachine(self.image)
        self.assertEqual(m.call('v3d_init', CONTEXT, OPS, TABLE, ARENA, 64, 64), 1)
        m.stall = True; m.clock = 0xffff0000
        self.assertEqual(m.call('v3d_clear', CONTEXT, 0, 0) & 0xffffffff, 0xffffffff)
        self.assertTrue(m.stop)
        self.assertEqual(m.call('v3d_pixels', CONTEXT, 0), 0)

    def test_freestanding_memcmp_unsigned_and_zero_length(self):
        m = Machine(self.image)
        m.uc.mem_write(STATS, b'\x00\x80\xffabcd')
        m.uc.mem_write(STATS + 128, b'\x00\x80\x00abcd')
        self.assertEqual(m.call('memcmp', STATS, STATS + 128, 0), 0)
        self.assertEqual(m.call('memcmp', STATS, STATS + 128, 2), 0)
        self.assertEqual(m.call('memcmp', STATS, STATS + 128, 3), 255)
        self.assertEqual(m.call('memcmp', STATS + 128, STATS, 3) & 0xffffffff, (-255) & 0xffffffff)


if __name__ == '__main__': unittest.main(argv=[sys.argv[0], *sys.argv[2:]])
