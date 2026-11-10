"""Boot the linked kernel to the UI against scripted V3D and firmware models.

Usage: python v3d_boot.py default-kernel.elf cpu-kernel.elf
The second ELF is built with V3D=0. The real boot coordinator, display,
renderer, hardware adapter, property mailbox and V3D driver run; secondary
cores, tasks, touch and the UART are stubbed. Device registers are modelled at
MMIO level. This selects backends in software only; it is not a Pi 4 test.
"""
import struct
import sys
import unittest

from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as regs
from memory_management import DTB, FRAMEBUFFER, Image, Machine, RETURN, fixture
from v3d_model import V3dDevice

WIDTH, HEIGHT = 800, 480
PAGE = WIDTH * HEIGHT * 4
MBOX_READ, MBOX_STATUS, MBOX_WRITE = 0xfe00b880, 0xfe00b898, 0xfe00b8a0
TIMER = 0xfe003004
STUBS = {'uart_init': 0, 'uart_writeText': 0, 'uart_writeHex': 0, 'secondary_core_start': 1,
         'touch_init': 1, 'irq_init_core': 1, 'parallel_init': 1, 'parallel_self_test': 1,
         'task_init': 1, 'task_self_test': 1, 'task_multicore_self_test': 1,
         'task_preemption_self_test': 1, 'process_create': 1}


class Board(Machine):
    """128 MiB machine with the mailbox, system timer and V3D behind MMIO."""

    def __init__(self, image):
        super().__init__(image)
        self.log, self.reached_ui, self.clock = [], False, 0
        self.mailbox_dead = False
        self.token = None
        self.offset = 0
        self.failed_allocations = ()
        self.allocations = 0
        self.device = V3dDevice(lambda address, size: bytes(self.uc.mem_read(address, size)),
                                lambda address, data: self.uc.mem_write(address, data))
        for base, size in ((0xfe003000, 0x1000), (0xfe00b000, 0x1000), (0xfe100000, 0x1000),
                           (0xfec00000, 0x12000)):
            self.uc.mem_map(base, size)
            self.uc.hook_add(UC_HOOK_MEM_READ, self.mmio_read, begin=base, end=base + size - 1)
            self.uc.hook_add(UC_HOOK_MEM_WRITE, self.mmio_write, begin=base, end=base + size - 1)
        for name in STUBS:
            address = image.symbols[name]
            self.uc.hook_add(UC_HOOK_CODE, self.stub, user_data=name, begin=address, end=address)
        for name, hook in (('mbox_call', self.framebuffer_call), ('mm_alloc_pages', self.allocation)):
            address = image.symbols[name]
            self.uc.hook_add(UC_HOOK_CODE, hook, begin=address, end=address)

    def stub(self, uc, address, size, name):
        if name == 'uart_writeText':
            pointer = uc.reg_read(regs.UC_ARM64_REG_X0)
            self.log.append(bytes(uc.mem_read(pointer, 200)).split(b'\0', 1)[0].decode())
        if name == 'uart_writeHex':
            self.log.append(f'{uc.reg_read(regs.UC_ARM64_REG_X0) & 0xffffffff:#010x}')
        if name == 'process_create':
            self.reached_ui = True
            uc.emu_stop()
            return
        uc.reg_write(regs.UC_ARM64_REG_X0, STUBS[name])
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    def framebuffer_call(self, uc, address, size, _):
        mailbox = self.image.symbols['mbox']
        request = list(struct.unpack('<36I', uc.mem_read(mailbox, 36 * 4)))
        if request[0] == 35 * 4:
            request[20], request[28], request[29], request[33] = 32, FRAMEBUFFER | 0xc0000000, PAGE * 3, WIDTH * 4
            uc.mem_write(mailbox, struct.pack('<36I', *request))
        else:
            self.offset = request[6]
        uc.reg_write(regs.UC_ARM64_REG_X0, 1)
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    def allocation(self, uc, address, size, _):
        self.allocations += 1
        if self.allocations in self.failed_allocations:
            uc.reg_write(regs.UC_ARM64_REG_X0, 0)
            uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    def mmio_read(self, uc, access, address, size, value, _):
        assert size == 4
        if address == TIMER:
            self.clock = (self.clock + 200) & 0xffffffff
            result = self.clock
        elif address == MBOX_STATUS: result = 0 if self.token is not None else 0x40000000
        elif address == MBOX_READ: result, self.token = self.token, None
        else: result = self.device.read(address)
        uc.mem_write(address, struct.pack('<I', result))

    def mmio_write(self, uc, access, address, size, value, _):
        assert size == 4
        value &= 0xffffffff
        if address != MBOX_WRITE:
            self.device.write(address, value)
            return
        # One property tag per request, as drivers/hw/property.c builds it.
        assert value & 15 == 8
        if self.mailbox_dead: return
        buffer = value & ~15
        words = list(struct.unpack('<8I', uc.mem_read(buffer, 32)))
        assert words[0] == 32 and words[1] == 0 and words[3] == 8 and words[4] == 0 and words[7] == 0
        words[5:7] = self.device.property(words[2], words[5:7])
        words[1], words[4] = 0x80000000, 0x80000008
        uc.mem_write(buffer, struct.pack('<8I', *words))
        self.token = value

    def boot(self):
        self.uc.mem_write(DTB, fixture())
        self.uc.reg_write(regs.UC_ARM64_REG_SP, self.image.symbols['__kernel_stack_top'])
        self.uc.reg_write(regs.UC_ARM64_REG_X0, DTB)
        self.uc.emu_start(self.image.symbols['main'], RETURN, timeout=300_000_000)
        assert not self.failure, self.failure
        assert self.reached_ui, 'boot did not reach application start'
        return ''.join(self.log)

    def free_pages(self): return self.stats()[2]

    def frame(self, color, box=None):
        """Clear the 480x800 canvas, optionally fill a box, present, and
        return (shown scanout page, expected pixels)."""
        self.call('canvas_set_color', color)
        self.call('canvas_clear')
        expected = bytearray(struct.pack('<I', color) * (WIDTH * HEIGHT))
        if box:
            x, y, w, h, fill = box
            self.call('canvas_set_color', fill)
            self.call('canvas_fill_rect', x, y, w, h)
            # The panel is rotated: canvas (x, y) is physical (y, 479 - x).
            row = struct.pack('<I', fill) * h
            for line in range(480 - x - w, 480 - x):
                start = (line * WIDTH + y) * 4
                expected[start:start + len(row)] = row
        self.call('canvas_present')
        page = FRAMEBUFFER + self.offset * WIDTH * 4
        return bytes(self.uc.mem_read(page, PAGE)), bytes(expected)


class BootTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.default, cls.cpu = Image(sys.argv[1]), Image(sys.argv[2])
        board = Board(cls.default)
        board.boot()
        cls.reserved = board.free_pages()  # Free pages while V3D holds its 6 MiB.

    def assert_cpu_session(self, board, jobs=0):
        """The UI keeps drawing correct frames without the device."""
        before = board.device.accesses
        for color, box in ((0x00123456, None), (0x00ff8040, (40, 100, 200, 300, 0x000000ff))):
            shown, expected = board.frame(color, box)
            self.assertEqual(shown, expected)
        self.assertEqual((board.device.render_jobs, board.device.accesses), (jobs, before))

    def test_working_device_boots_to_ui_on_v3d(self):
        board = Board(self.default)
        log = board.boot()
        self.assertIn('[OK] V3D self-test passed; V3D backend selected (clear, present)', log)
        self.assertNotIn('[ERR]', log)
        self.assertNotIn('CPU rendering backend selected', log)
        self.assertIn('[OK] Vantage booted!', log)
        self.assertTrue(board.device.powered())
        self.assertEqual(board.device.render_jobs, 3)
        # The self-test target was the hidden page the first present reuses.
        self.assertEqual(board.device.presented, [FRAMEBUFFER + PAGE])
        shown, expected = board.frame(0x00123456)
        self.assertEqual(shown, expected)
        self.assertEqual(board.device.render_jobs, 5)  # One clear and one present job.
        self.assertEqual(board.device.presented[-1], FRAMEBUFFER + PAGE)
        # CPU-drawn shapes share the surface and are presented by the device.
        shown, expected = board.frame(0x00ff8040, (40, 100, 200, 300, 0x000000ff))
        self.assertEqual(shown, expected)
        self.assertEqual(board.device.render_jobs, 7)
        self.assertEqual(board.device.presented[-1], FRAMEBUFFER + 2 * PAGE)
        self.assertFalse(board.device.target_mapped())

    def test_absent_or_broken_device_boots_to_ui_on_cpu(self):
        cases = (('exists', False, 'identity (-3)'), ('domain_missing', True, 'power domain (-2)'),
                 ('bridge_stuck', True, 'reset bridge (-1)'), ('stall', True, 'clear job (-1)'),
                 ('corrupt', True, 'pixel verification (-5)'))
        for fault, value, reason in cases:
            with self.subTest(fault=fault):
                board = Board(self.default)
                setattr(board.device, fault, value)
                log = board.boot()
                self.assertIn('[ERR] V3D unavailable: ' + reason, log)
                self.assertIn('[INFO] CPU rendering backend selected', log)
                self.assertNotIn('V3D backend selected', log)
                self.assertIn('[OK] Vantage booted!', log)
                self.assertTrue(board.device.off())
                # Both page runs went back to the allocator.
                self.assertEqual(board.free_pages(), self.reserved + (6 << 20) // 4096)
                jobs = board.device.render_jobs
                board.device.stall = board.device.corrupt = False
                self.assert_cpu_session(board, jobs)

    def test_dead_mailbox_and_allocation_failure_fall_back(self):
        board = Board(self.default)
        board.mailbox_dead = True
        log = board.boot()
        self.assertIn('[ERR] V3D unavailable: power domain (-1)', log)
        self.assertEqual(board.device.accesses, 0)
        self.assert_cpu_session(board)
        for failed in (1, 2):
            with self.subTest(failed_allocation=failed):
                board = Board(self.default)
                board.failed_allocations = (failed,)
                log = board.boot()
                self.assertIn('[ERR] V3D unavailable (-7)', log)
                self.assertIn('[INFO] CPU rendering backend selected', log)
                self.assertEqual((board.device.accesses, board.device.properties), (0, 0))
                self.assertEqual(board.free_pages(), self.reserved + (6 << 20) // 4096)
                self.assert_cpu_session(board)

    def test_runtime_failure_keeps_the_frame_and_continues_on_cpu(self):
        for operation in ('clear', 'present'):
            with self.subTest(operation=operation):
                board = Board(self.default)
                board.boot()
                reserved = board.free_pages()
                shown, expected = board.frame(0x00224466, (10, 20, 100, 50, 0x00ffffff))
                self.assertEqual(shown, expected)
                board.log.clear()
                if operation == 'clear':
                    board.device.stall = True
                    shown, expected = board.frame(0x00a1b2c3, (300, 500, 64, 64, 0x00010203))
                else:
                    # The frame is already drawn when the device stops answering.
                    board.call('canvas_set_color', 0x00556677)
                    board.call('canvas_fill_rect', 0, 0, 480, 400)
                    board.device.stall = True
                    board.call('canvas_present')
                    page = FRAMEBUFFER + board.offset * WIDTH * 4
                    shown = bytes(board.uc.mem_read(page, PAGE))
                    row = struct.pack('<I', 0x00556677) * 400 + struct.pack('<I', 0x00224466) * 400
                    expected = row * HEIGHT
                self.assertEqual(shown, expected)
                log = ''.join(board.log)
                self.assertIn(f'[ERR] V3D stopped during {operation} job (-1)', log)
                self.assertIn('[INFO] CPU rendering backend selected; current frame kept', log)
                self.assertTrue(board.device.off())
                self.assertEqual(board.free_pages(), reserved + (6 << 20) // 4096)
                self.assert_cpu_session(board, board.device.render_jobs)

    def test_unconfirmed_stop_quarantines_gpu_memory(self):
        board = Board(self.default)
        board.boot()
        reserved = board.free_pages()
        board.log.clear()
        board.device.stall = board.device.bridge_stuck = True
        shown, expected = board.frame(0x00010203)
        self.assertEqual(shown, expected)
        self.assertIn('[WARN] V3D stop unconfirmed', ''.join(board.log))
        self.assertEqual(board.free_pages(), reserved)
        self.assert_cpu_session(board, board.device.render_jobs)

    def test_cpu_build_never_touches_the_device(self):
        board = Board(self.cpu)
        log = board.boot()
        self.assertIn('[INFO] V3D disabled at build time (V3D=0); CPU rendering backend selected', log)
        self.assertNotIn('V3D self-test', log)
        self.assertEqual((board.device.accesses, board.device.properties, board.allocations), (0, 0, 0))
        self.assert_cpu_session(board)


if __name__ == '__main__':
    if len(sys.argv) < 3: raise SystemExit(__doc__)
    unittest.main(argv=[sys.argv[0], *sys.argv[3:]])
