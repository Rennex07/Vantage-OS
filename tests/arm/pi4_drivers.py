"""Linked ARM V3D/ABI checks with a scripted device model, not a Pi test."""
import struct
import sys
import unittest

from unicorn import UC_HOOK_CODE
from unicorn import arm64_const as regs
from memory_management import Image, Machine, STATS
from v3d_model import SURFACE, V3dDevice

CONTEXT, OPS = 0x7000000, 0x7001000
CALLBACK = 0x7100000
TABLE, ARENA, TARGET = 0x1000000, 0x1400000, 0x1800000
GPU = 0x1000000
PIXELS = 64 * 64 * 4
STATE, ERROR, STAGE = CONTEXT + 40, CONTEXT + 44, CONTEXT + 48


class GpuMachine(Machine):
    def __init__(self, image):
        super().__init__(image)
        self.clock, self.owner = 0, True
        self.device = V3dDevice(lambda address, size: bytes(self.uc.mem_read(address, size)),
                                lambda address, data: self.uc.mem_write(address, data))
        callbacks = ['read', 'write', 'time', 'owned', 'cache', 'physical', 'property']
        for i, name in enumerate(callbacks):
            address = CALLBACK + i * 4
            self.uc.hook_add(UC_HOOK_CODE, self.callback, user_data=name, begin=address, end=address)
        self.uc.mem_write(OPS, struct.pack('<8Q', 0, *[CALLBACK + i * 4 for i in range(7)]))

    def word(self, address): return struct.unpack('<I', self.uc.mem_read(address, 4))[0]
    def signed(self, address): return struct.unpack('<i', self.uc.mem_read(address, 4))[0]

    def callback(self, uc, address, size, name):
        args = [uc.reg_read(getattr(regs, f'UC_ARM64_REG_X{i}')) for i in range(5)]
        result = 0
        if name == 'owned': result = self.owner
        elif name == 'physical': result = args[1]
        elif name == 'time': self.clock = (self.clock + 10000) & 0xffffffff; result = self.clock
        elif name == 'property':
            assert args[3] == 2
            values = self.device.property(args[1], list(struct.unpack('<2I', uc.mem_read(args[2], 8))))
            uc.mem_write(args[2], struct.pack('<2I', *values)); result = 1
        elif name == 'read': result = self.device.read(args[1])
        elif name == 'write': self.device.write(args[1], args[2] & 0xffffffff)
        elif name == 'cache':
            assert TABLE <= args[1] and args[1] + args[2] <= TARGET + PIXELS
        uc.reg_write(regs.UC_ARM64_REG_X0, int(result))
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    def start(self):
        assert self.call('v3d_init', CONTEXT, OPS, TABLE, ARENA, 64, 64) == 1
        assert self.call('v3d_self_test', CONTEXT, TARGET) == 1


class DriverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image = Image(sys.argv[1])

    def test_linked_gpu_mmu_commands_and_counter_wrap(self):
        m = GpuMachine(self.image)
        self.assertEqual(m.call('v3d_init', CONTEXT, OPS, TABLE, ARENA, 64, 64), 1)
        self.assertEqual(m.word(TABLE + (GPU >> 12) * 4), (ARENA >> 12) | 0x30000000)
        self.assertEqual(m.word(TABLE), 0)
        self.assertEqual(m.word(TABLE + ((GPU + 0x3000) >> 12) * 4), 0)
        self.assertTrue(m.device.powered() and m.device.domain and m.device.clock)
        self.assertEqual(m.call('v3d_self_test', CONTEXT, TARGET), 1)
        self.assertEqual(m.uc.mem_read(ARENA + SURFACE, PIXELS), b'\0' * PIXELS)
        # Clear, present, clear: both 8-bit counters started at 255.
        self.assertEqual((m.device.bfc, m.device.rfc, m.device.render_jobs), (2, 2, 3))
        self.assertEqual(m.device.presented, [TARGET])
        self.assertFalse(m.device.target_mapped())
        frame = bytes(range(256)) * (PIXELS // 256)
        m.uc.mem_write(ARENA + SURFACE, frame)
        self.assertEqual(m.call('v3d_present', CONTEXT, TARGET), 1)
        self.assertEqual(bytes(m.uc.mem_read(TARGET, PIXELS)), frame)
        self.assertFalse(m.device.target_mapped())
        self.assertEqual(m.call('v3d_clear', CONTEXT, 0x80402010), 1)
        self.assertEqual(bytes(m.uc.mem_read(ARENA + SURFACE, PIXELS)), struct.pack('<I', 0x80402010) * (PIXELS // 4))
        self.assertEqual(m.call('v3d_surface', CONTEXT), ARENA + SURFACE)
        self.assertEqual(m.call('v3d_stop', CONTEXT) & 0xffffffff, 0xffffffff)
        self.assertTrue(m.device.off())

    def test_linked_gpu_timeout_resets_and_releases(self):
        m = GpuMachine(self.image)
        m.start()
        m.device.stall = True; m.clock = 0xffff0000
        self.assertEqual(m.call('v3d_clear', CONTEXT, 0) & 0xffffffff, 0xffffffff)
        self.assertEqual((m.signed(STATE), m.signed(ERROR), m.word(STAGE)), (-1, -1, 7))
        self.assertTrue(m.device.off() and m.device.resets == 1)
        self.assertEqual(m.call('v3d_surface', CONTEXT), 0)
        self.assertEqual(m.call('v3d_clear', CONTEXT, 0) & 0xffffffff, (-6) & 0xffffffff)

    def test_linked_gpu_bring_up_failures_stop_the_device(self):
        for fault, stage, error in (('exists', 4, -3), ('domain_missing', 1, -2), ('bridge_stuck', 3, -1)):
            with self.subTest(fault=fault):
                m = GpuMachine(self.image)
                setattr(m.device, fault, fault != 'exists')
                self.assertEqual(m.call('v3d_init', CONTEXT, OPS, TABLE, ARENA, 64, 64) & 0xffffffff,
                                 error & 0xffffffff)
                self.assertEqual((m.signed(STATE), m.signed(ERROR), m.word(STAGE)), (-1, error, stage))
                self.assertTrue(m.device.off())
                self.assertEqual(m.device.bin_jobs, 0)
        m = GpuMachine(self.image)
        self.assertEqual(m.call('v3d_init', CONTEXT, OPS, TABLE, ARENA, 64, 64), 1)
        m.device.corrupt = True
        self.assertEqual(m.call('v3d_self_test', CONTEXT, TARGET) & 0xffffffff, (-5) & 0xffffffff)
        self.assertEqual((m.signed(STATE), m.word(STAGE)), (-1, 9))
        self.assertTrue(m.device.off())
    def test_freestanding_memcmp_unsigned_and_zero_length(self):
        m = Machine(self.image)
        m.uc.mem_write(STATS, b'\x00\x80\xffabcd')
        m.uc.mem_write(STATS + 128, b'\x00\x80\x00abcd')
        self.assertEqual(m.call('memcmp', STATS, STATS + 128, 0), 0)
        self.assertEqual(m.call('memcmp', STATS, STATS + 128, 2), 0)
        self.assertEqual(m.call('memcmp', STATS, STATS + 128, 3), 255)
        self.assertEqual(m.call('memcmp', STATS + 128, STATS, 3) & 0xffffffff, (-255) & 0xffffffff)


if __name__ == '__main__': unittest.main(argv=[sys.argv[0], *sys.argv[2:]])
