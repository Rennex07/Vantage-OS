"""Run linked terminal/app/syscall code with mocked exceptions and devices.

Usage: python tests/arm/terminal.py path/to/kernel8.elf -v
Requires the same Unicorn/Capstone dependencies as memory_management.py.
The SVC handoff is modeled here; this does not prove ARM privilege enforcement.
"""
import struct
import sys
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as regs

from memory_management import Image, Machine, RETURN

TRAP_RETURN = RETURN + 0x100


class AppMachine(Machine):
    def __init__(self, image):
        super().__init__(image)
        self.keys, self.touches, self.rows, self.output = [], [], {}, bytearray()
        self.buttons = []
        self.clock_us = 0
        self.trap_context = None
        self.stack_writes = []
        self.presentations = 0
        self.user_bottom = image.symbols['user_stack']
        self.user_top = self.user_bottom + 4096
        self.uc.mem_write(self.user_bottom, b'\xa7' * 64)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, lambda uc, access, a, s, v, _: self.stack_writes.append(a),
                         begin=self.user_bottom, end=self.user_top - 1)
        self.uc.hook_add(UC_HOOK_CODE, self.trap_return, begin=TRAP_RETURN, end=TRAP_RETURN)
        decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        decoder.skipdata = True
        for ins in decoder.disasm(image.section_bytes(image.named['.text']), image.named['.text'][3]):
            if ins.mnemonic == 'svc':
                self.uc.hook_add(UC_HOOK_CODE, self.trap, begin=ins.address, end=ins.address)
        for name in ('key_read', 'touch_read', 'canvas_button', 'canvas_text_row', 'canvas_text_columns', 'canvas_text_rows',
                     'canvas_clear', 'canvas_present', 'fb_wait_frame', 'time_us', 'uart_write_bytes'):
            address = image.symbols[name]
            self.uc.hook_add(UC_HOOK_CODE, self.device, user_data=name, begin=address, end=address)

    def trap(self, uc, address, size, _):
        if self.trap_context is not None:
            raise AssertionError('Nested SVC')
        self.trap_context = (uc.context_save(), address + size, self.level, self.daif)
        argument = uc.reg_read(regs.UC_ARM64_REG_X0)
        call = uc.reg_read(regs.UC_ARM64_REG_X8)
        self.level, self.daif = 4, 0x3c0
        uc.reg_write(regs.UC_ARM64_REG_SP, self.image.symbols['__kernel_stack_top'])
        uc.reg_write(regs.UC_ARM64_REG_X0, call)
        uc.reg_write(regs.UC_ARM64_REG_X1, argument)
        uc.reg_write(regs.UC_ARM64_REG_X30, TRAP_RETURN)
        uc.reg_write(regs.UC_ARM64_REG_PC, self.image.symbols['handle_sys_call'])

    def trap_return(self, uc, address, size, _):
        result = uc.reg_read(regs.UC_ARM64_REG_X0)
        context, pc, self.level, self.daif = self.trap_context
        self.trap_context = None
        uc.context_restore(context)
        uc.reg_write(regs.UC_ARM64_REG_X0, result)
        uc.reg_write(regs.UC_ARM64_REG_PC, pc)

    def device(self, uc, address, size, name):
        arg = uc.reg_read(regs.UC_ARM64_REG_X0)
        result = 0
        if name == 'canvas_text_columns': result = 60
        elif name == 'canvas_text_rows': result = 50
        elif name == 'key_read':
            event = self.keys.pop(0) if self.keys else (0, 0)
            uc.mem_write(arg, struct.pack('<2I', *event))
            result = bool(event[0])
        elif name == 'touch_read':
            event = self.touches.pop(0) if self.touches else (0, 0, 0)
            uc.mem_write(arg, struct.pack('<Iii', *event))
            result = bool(event[0])
        elif name == 'canvas_text_row':
            row, length, fg, bg, cursor, text = struct.unpack('<5I96s', uc.mem_read(arg, 116))
            assert row < 50 and length == 60
            self.rows[row] = text[:length].decode('ascii').rstrip()
            result = 1
        elif name == 'canvas_button':
            button = struct.unpack('<8I8s', uc.mem_read(arg, 40))
            assert button[0] + button[2] <= 480 and button[1] + button[3] <= 800
            self.buttons.append(button)
            result = 1
        elif name == 'canvas_present': self.presentations += 1
        elif name == 'time_us': result = self.clock_us & 0xffffffff
        elif name == 'canvas_clear': self.rows.clear()
        elif name == 'uart_write_bytes':
            self.output.extend(uc.mem_read(arg, uc.reg_read(regs.UC_ARM64_REG_X1)))
        uc.reg_write(regs.UC_ARM64_REG_X0, int(result))
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    def call_user(self, name):
        self.level, self.daif = 0, 0
        self.uc.reg_write(regs.UC_ARM64_REG_SP, self.user_top)
        self.uc.reg_write(regs.UC_ARM64_REG_X30, RETURN)
        self.uc.emu_start(self.image.symbols[name], RETURN + 4, timeout=30000000, count=80000000)
        assert self.uc.reg_read(regs.UC_ARM64_REG_PC) == RETURN
        assert self.trap_context is None
        assert self.uc.mem_read(self.user_bottom, 64) == b'\xa7' * 64
        assert min(self.stack_writes) >= self.user_bottom + 64

    def command(self, text):
        self.keys.extend((1, ord(c)) for c in text)
        self.keys.append((2, 0))
        while self.keys: self.call_user('terminal_app_step')


class TerminalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image = Image(sys.argv[1])

    def test_linked_app_uses_user_stack_and_real_kernel_stats(self):
        m = AppMachine(self.image)
        m.initialize()
        m.call_user('terminal_app_init')
        m.call_user('terminal_app_step')
        self.assertEqual(m.rows[0], 'Vantage terminal')
        self.assertEqual(m.rows[3], '>')
        m.command('help')
        self.assertIn('Show commands', m.output.decode())
        m.command('mem')
        self.assertIn('RAM: 128 MiB', m.output.decode())
        self.assertIn('Heap used: 0/8388608 bytes', m.output.decode())
        self.assertNotIn('Simulator', m.output.decode())
        m.command('clear')
        self.assertEqual(m.rows[0], '>')
        self.assertTrue(all(not text for row, text in m.rows.items() if 0 < row < 33))
        self.assertTrue(any(b[-1].startswith(b'Shift') for b in m.buttons))
        # Tap H, E, L, P and ENTER through the linked touch syscall path.
        for x, y in ((286, 661), (124, 611), (425, 661), (448, 611), (425, 761)):
            m.touches.extend(((1, x, y), (3, x, y)))
        m.call_user('terminal_app_step')
        self.assertIn('help   Show commands', m.output.decode())
        self.assertGreater(m.presentations, 0)

    def test_delete_hold_delay_repeat_release_drag_and_clock_wrap(self):
        m = AppMachine(self.image)
        m.initialize()
        m.call_user('terminal_app_init')
        m.command('clear')
        m.keys.extend((1, ord(c)) for c in 'abcdef')
        m.clock_us = 0xfffffff0
        m.touches.append((1, 435, 711))
        m.call_user('terminal_app_step')
        self.assertEqual(m.rows[0], '> abcde')  # First delete is immediate.
        for elapsed, expected in ((399999, '> abcde'), (1, '> abcd'),
                                  (69999, '> abcd'), (1, '> abc'), (1000000, '> ab')):
            m.clock_us += elapsed
            m.call_user('terminal_app_step')
            self.assertEqual(m.rows[0], expected)
        m.touches.append((3, 435, 711))
        m.clock_us += 1000000
        m.call_user('terminal_app_step')
        self.assertEqual(m.rows[0], '> ab')  # No release delete or late repeat.
        m.touches.append((1, 435, 711))
        m.call_user('terminal_app_step')
        self.assertEqual(m.rows[0], '> a')
        m.touches.append((2, 0, 500))
        m.clock_us += 1000000
        m.call_user('terminal_app_step')
        self.assertEqual(m.rows[0], '> a')  # Drag out stops repeat.
        m.touches.append((3, 435, 711))
        m.call_user('terminal_app_step')
        self.assertEqual(m.rows[0], '> a')
        m.touches.append((1, 435, 711))
        m.call_user('terminal_app_step')
        for _ in range(3):
            m.clock_us += 500000
            m.call_user('terminal_app_step')
            self.assertEqual(m.rows[0], '>')  # Empty input stays bounded.

    def test_linked_syscall_rejects_nonuser_buffers(self):
        m = AppMachine(self.image)
        start, end = self.image.symbols['__user_start'], self.image.symbols['__user_end']
        for call in (6, 7, 8, 10, 11):
            for pointer in (0, self.image.symbols['__kernel_stack_top'] - 256, start + 1, end - 4):
                with self.subTest(call=call, pointer=hex(pointer)):
                    self.assertEqual(m.call('handle_sys_call', call, pointer), 0)
        self.assertEqual(m.output, b'')
        self.assertEqual(m.rows, {})

    def test_freestanding_copy_and_zero_handle_unaligned_buffers(self):
        m = Machine(self.image)
        source, target = 0x7000001, 0x7001003
        data = bytes(range(256))
        m.uc.mem_write(source, data)
        for size in (0, 1, 7, 8, 31, 65, 132, 256):
            with self.subTest(size=size):
                m.uc.mem_write(target - 1, b'\xa5' * 258)
                self.assertEqual(m.call('memcpy', target, source, size), target)
                self.assertEqual(m.uc.mem_read(target, size), data[:size])
                self.assertEqual(m.uc.mem_read(target - 1, 1), b'\xa5')
                self.assertEqual(m.uc.mem_read(target + size, 1), b'\xa5')
                self.assertEqual(m.call('memset', target, 0x123, size), target)
                self.assertEqual(m.uc.mem_read(target, size), b'\x23' * size)
                self.assertEqual(m.uc.mem_read(target + size, 1), b'\xa5')


if __name__ == '__main__':
    if len(sys.argv) < 2: raise SystemExit(__doc__)
    unittest.main(argv=[sys.argv[0]] + sys.argv[2:])
