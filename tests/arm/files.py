"""Linked persistent-file regressions: kernel file services and EL0 file syscalls.

Usage: python tests/arm/files.py path/to/fresh/kernel8.elf -v
The file layer is driven directly on the scripted machine. File system calls
come from isolated EL0 processes on the CPU model: hand-assembled programs and
the embedded terminal. No SD operations or physical Pi hardware are exercised.
"""
import struct
import sys
import unittest

from a64 import Program
from cpu import LOW, PROGRAM_BASE, USER_BASE, PAGE, OK, image
from memory_management import Image
from tasks import signed
from file_storage import Disk, FileMachine
from processes import ProcessMachine
from terminal import TerminalMachine

OPEN, READ, WRITE, SEEK, CLOSE, LIST, REMOVE = range(32, 39)
BAD, NOT_FOUND, NO_SPACE, ACCESS, BAD_HANDLE = range(-1, -6, -1)
YIELD, EXIT = 17, 18
DATA = USER_BASE + PAGE
KERNEL_BUFFER = 0x7400000
FIRST_HANDLE = 33   # Generation 1 of handle slot 0.


class FileProcessMachine(Disk, ProcessMachine):
    ops, callback = 0x7bfe000, LOW + 0x100

    def __init__(self, image):
        super().__init__(image)
        self.attach_disk()
        assert self.mount_files() == 0

    def run_program(self, program, data):
        process = self.create_process(image(program.code(), data=data))
        assert process and self.run_all() == OK
        return process


class FileTerminalMachine(Disk, TerminalMachine):
    ops, callback = 0x7bfe000, LOW + 0x100


def payload(offset, blob, data):
    data[offset:offset + len(blob)] = blob


def call(p, number, offset, result):
    """Issue a file call on the payload at DATA + offset; keep x0 at DATA + result."""
    return p.syscall(number, DATA + offset).str(0, 19, result)


class FileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image = Image(sys.argv[1])

    def setUp(self):
        self.m = FileMachine(self.image)
        self.m.start()
        self.assertEqual(self.m.mount_files(), 0)
        self.path = KERNEL_BUFFER
        self.input, self.output, self.entry = self.path + 64, self.path + 128, self.path + 256
        self.request = self.path + 512
        self.m.uc.mem_write(self.path, b'/ram/hello\0')
        self.m.uc.mem_write(self.input, b'hello')

    def kernel_read(self, m, path=b'/ram/hello'):
        """File contents through the kernel file layer, from boot context."""
        call = getattr(m, 'interject', m.call)
        m.uc.mem_write(KERNEL_BUFFER, path + b'\0')
        handle = call('files_open', 99, KERNEL_BUFFER, 1)
        if signed(handle) < 0:
            return signed(handle)
        count = call('files_read', 99, handle, KERNEL_BUFFER + 4096, 256)
        self.assertEqual(call('files_close', 99, handle), 0)
        return bytes(m.uc.mem_read(KERNEL_BUFFER + 4096, count))

    def test_el0_file_calls_round_trip_through_copied_payloads(self):
        m = FileProcessMachine(self.image)
        data = bytearray(0xf00)
        payload(0x000, struct.pack('<II48s', 7, 0, b'/ram/hello'), data)
        payload(0x100, struct.pack('<QII256s', 0, 5, 0, b'hello'), data)
        payload(0x300, struct.pack('<QII', 0, 0, 0), data)
        payload(0x400, struct.pack('<QII256s', 0, 257, 0, b''), data)          # Too long.
        payload(0x600, struct.pack('<QII256s', 0, 3, 1, b''), data)            # Reserved field.
        payload(0x800, struct.pack('<QII256s', 0, 256, 0, b'\xcc' * 256), data)
        payload(0xa00, struct.pack('<II48s', 0, 0, b''), data)
        payload(0xb00, struct.pack('<48s', b'/ram/hello'), data)
        p = Program(PROGRAM_BASE)
        p.mov(19, DATA)
        call(p, OPEN, 0x000, 0xc00).mov_reg(20, 0)
        for offset in (0x100, 0x300, 0x400, 0x600, 0x800):
            p.str(20, 19, offset)
        call(p, WRITE, 0x100, 0xc08)
        call(p, SEEK, 0x300, 0xc10)
        call(p, READ, 0x400, 0xc18)
        call(p, READ, 0x600, 0xc20)
        call(p, READ, 0x800, 0xc28)
        call(p, LIST, 0xa00, 0xc30)
        p.mov_reg(0, 20).mov(8, CLOSE).svc().str(0, 19, 0xc38)
        call(p, REMOVE, 0xb00, 0xc40)
        call(p, READ, 0x800, 0xc48)                                           # Closed handle.
        p.syscall(EXIT, 0)
        process = m.run_program(p, bytes(data))
        results = [signed(value) for value in struct.unpack('<10Q', m.user_read(process, DATA + 0xc00, 80))]
        self.assertEqual(results[0], FIRST_HANDLE)
        self.assertEqual(results[1:], [5, 0, BAD, BAD, 5, 1, 0, 0, BAD_HANDLE])
        handle, length, reserved, text = struct.unpack('<QII256s', m.user_read(process, DATA + 0x800, 272))
        self.assertEqual((handle, length, reserved, text), (FIRST_HANDLE, 5, 0, b'hello' + bytes(251)))
        # Rejected reads left their payloads untouched.
        self.assertEqual(struct.unpack('<QII', m.user_read(process, DATA + 0x400, 16)), (FIRST_HANDLE, 257, 0))
        cursor, size, path = struct.unpack('<II48s', m.user_read(process, DATA + 0xa00, 56))
        self.assertEqual((cursor, size, path.rstrip(b'\0')), (1, 5, b'/ram/hello'))
        self.assertEqual(self.kernel_read(m), NOT_FOUND)
        self.assertEqual(m.reap(process), (OK, 0))
        self.assertEqual(m.free_pages(), m.baseline)

    def test_terminal_editor_saves_from_el0_through_file_syscalls(self):
        m = FileTerminalMachine(self.image)
        m.boot(cores=1)
        m.attach_disk()
        self.assertEqual(m.mount_files(), 0)
        m.launch(booted=True)
        m.command('edit hello')
        self.assertEqual(m.rows[0], 'Edit   /ram/hello')
        m.command('written by terminal')
        # Tap Save, then Exit, on the editor's control row above the keyboard.
        for x in (30, 150):
            m.touches.extend(((1, x, 440), (3, x, 440)))
            m.step()
        self.assertIn('Saved.', m.output.decode())
        self.assertEqual(m.rows[0], 'Editor closed.')
        self.assertEqual(self.kernel_read(m), b'written by terminal\n')
        m.command('cat hello')
        self.assertIn('written by terminal', m.output.decode().split('Editor closed.')[-1])
        # The process owns no handle any more: unmounting is allowed.
        self.assertEqual(m.interject('files_unmount'), 0)

    def test_handles_belong_to_one_process_and_are_released_when_it_exits(self):
        m = FileProcessMachine(self.image)
        opener = Program(PROGRAM_BASE)
        opener.mov(19, DATA)
        call(opener, OPEN, 0x000, 0x400).str(0, 19, 0x100)
        call(opener, WRITE, 0x100, 0x408)
        opener.syscall(YIELD).syscall(EXIT, 0)           # Exits without closing.
        data = bytearray(0x500)
        payload(0x000, struct.pack('<II48s', 7, 0, b'/ram/hello'), data)
        payload(0x100, struct.pack('<QII256s', 0, 5, 0, b'hello'), data)
        thief = Program(PROGRAM_BASE)
        thief.mov(19, DATA)
        call(thief, READ, 0x100, 0x400)                  # Another process's handle value.
        thief.syscall(YIELD)
        call(thief, OPEN, 0x000, 0x408)
        thief.syscall(EXIT, 0)
        stolen = bytearray(0x500)
        payload(0x000, struct.pack('<II48s', 1, 0, b'/ram/hello'), stolen)
        payload(0x100, struct.pack('<QII256s', FIRST_HANDLE, 5, 0, b''), stolen)
        first = m.create_process(image(opener.code(), data=bytes(data)))
        second = m.create_process(image(thief.code(), data=bytes(stolen)))
        self.assertEqual(m.run_all(), OK)
        self.assertEqual(struct.unpack('<2Q', m.user_read(first, DATA + 0x400, 16)), (FIRST_HANDLE, 5))
        read, opened = (signed(v) for v in struct.unpack('<2Q', m.user_read(second, DATA + 0x400, 16)))
        self.assertEqual(read, BAD_HANDLE)
        self.assertGreater(opened, 0)
        self.assertEqual(self.kernel_read(m), b'hello')
        # Both exited without closing; no live handle may block removal.
        m.uc.mem_write(KERNEL_BUFFER, b'/ram/hello\0')
        self.assertEqual(m.call('files_remove', 99, KERNEL_BUFFER), 0)
        self.assertEqual((m.reap(first), m.reap(second)), ((OK, 0), (OK, 0)))

    def test_el0_bad_paths_flags_and_reserved_fields_do_not_create_files(self):
        m = FileProcessMachine(self.image)
        data = bytearray(0x500)
        cases = ((b'/ram/../evil', 7, 0), (b'/ram/' + b'x' * 40, 7, 0), (b'/ram/hello', 0x100, 0), (b'/ram/hello', 7, 1))
        p = Program(PROGRAM_BASE)
        p.mov(19, DATA)
        for index, (path, flags, reserved) in enumerate(cases):
            payload(index * 0x40, struct.pack('<II48s', flags, reserved, path), data)
            call(p, OPEN, index * 0x40, 0x400 + index * 8)
        p.syscall(EXIT, 0)
        process = m.run_program(p, bytes(data))
        self.assertEqual([signed(v) for v in struct.unpack('<4Q', m.user_read(process, DATA + 0x400, 32))], [BAD] * 4)
        self.assertEqual(self.kernel_read(m), NOT_FOUND)
        self.assertEqual(m.reap(process), (OK, 0))

    def test_file_calls_from_kernel_callers_never_dereference_their_argument(self):
        # No calling process means no address space the request could come from.
        request = struct.pack('<II48s', 7, 0, b'/ram/hello')
        self.m.uc.mem_write(self.request, request)
        for address in (0, self.request, self.request + 1, self.image.symbols['__kernel_stack_bottom'],
                        USER_BASE, 0xfffffffffffffff0):
            for call_id in (OPEN, READ, WRITE, SEEK, LIST, REMOVE):
                self.assertEqual(signed(self.m.call('handle_file_call', call_id, address, 1)), BAD)
        self.assertEqual(signed(self.m.call('files_open', 1, self.path, 1)), NOT_FOUND)
        self.assertEqual(signed(self.m.call('handle_sys_call', OPEN, self.request)), -7)

    def test_reboot_probe_recovers_committed_empty_creation_after_power_loss(self):
        self.m.uc.mem_write(self.path, b'/ram/.s1-reboot\0')
        handle = self.m.call('files_open', 1, self.path, 6)
        self.assertGreater(handle, 0)
        # Drop all volatile state after create committed, before the first write.
        reboot = FileMachine(self.image, self.m.disk)
        reboot.start()
        self.assertEqual(reboot.mount_files(), 0)
        self.assertEqual(reboot.call('storage_reboot_probe'), 0)
        again = FileMachine(self.image, reboot.disk)
        again.start()
        self.assertEqual(again.mount_files(), 0)
        self.assertEqual(again.call('storage_reboot_probe'), 1)
        self.assertEqual(again.call('files_unmount'), 0)  # No leaked probe handles.

    def test_reboot_probe_does_not_repair_nonempty_corruption(self):
        for length in (1, 255, 256, 257):
            with self.subTest(length=length):
                m = FileMachine(self.image)
                m.start()
                self.assertEqual(m.mount_files(), 0)
                m.uc.mem_write(self.path, b'/ram/.s1-reboot\0')
                handle = m.call('files_open', 1, self.path, 6)
                m.uc.mem_write(self.input, b'x' * length)
                self.assertEqual(m.call('files_write', 1, handle, self.input, length), length)
                self.assertEqual(m.call('files_close', 1, handle), 0)
                before = bytes(m.disk)
                self.assertEqual(signed(m.call('storage_reboot_probe')), -8)
                self.assertEqual(bytes(m.disk), before)
                self.assertEqual(m.call('files_unmount'), 0)

    def test_full_file_failed_write_is_atomic_and_seek_has_no_holes(self):
        h = self.m.call('files_open', 1, self.path, 7)
        data = self.path + 4096
        self.m.uc.mem_write(data, b'z' * 4096)
        self.assertEqual(self.m.call('files_write', 1, h, data, 4096), 4096)
        self.assertEqual(signed(self.m.call('files_write', 1, h, data, 1)), NO_SPACE)
        self.assertEqual(signed(self.m.call('files_seek', 1, h, 4097)), BAD)
        self.assertEqual(self.m.call('files_seek', 1, h, 0), 0)
        self.assertEqual(self.m.call('files_read', 1, h, data, 4096), 4096)
        self.assertEqual(self.m.uc.mem_read(data, 4096), b'z' * 4096)

    def test_removed_data_is_not_exposed_by_reused_slots(self):
        h = self.m.call('files_open', 1, self.path, 7)
        self.assertEqual(self.m.call('files_write', 1, h, self.input, 5), 5)
        self.assertEqual(self.m.call('files_close', 1, h), 0)
        self.assertEqual(self.m.call('files_remove', 1, self.path), 0)
        new = self.m.call('files_open', 2, self.path, 7)
        self.assertNotEqual(h, new)
        self.assertEqual(signed(self.m.call('files_read', 2, h, self.output, 8)), BAD_HANDLE)
        self.assertEqual(self.m.call('files_read', 2, new, self.output, 8), 0)
        self.assertEqual(self.m.call('files_list', 2, self.entry), 1)
        self.assertEqual(struct.unpack('<II', self.m.uc.mem_read(self.entry, 8))[1], 0)

    def test_handle_generation_exhaustion_retires_slot(self):
        generation = self.image.symbols['open_files'] + 16
        self.m.uc.mem_write(generation, struct.pack('<Q', 0x7fffffffffffffff >> 5))
        h = self.m.call('files_open', 1, self.path, 7)
        self.assertGreater(h, 0)
        self.assertEqual(h & 31, 2)

    def test_files_survive_fresh_kernel_state_and_detect_corruption(self):
        h = self.m.call('files_open', 1, self.path, 7)
        self.assertEqual(self.m.call('files_write', 1, h, self.input, 5), 5)
        reboot = FileMachine(self.image, self.m.disk)
        self.assertEqual(reboot.mount_files(), 0)
        reboot.uc.mem_write(self.path, b'/ram/hello\0')
        h = reboot.call('files_open', 1, self.path, 1)
        self.assertEqual(reboot.call('files_read', 1, h, self.output, 16), 5)
        self.assertEqual(reboot.uc.mem_read(self.output, 5), b'hello')
        reboot.disk[2048 * 512] ^= 1
        reboot.disk[(2048 + 145) * 512] ^= 1
        broken = FileMachine(self.image, reboot.disk)
        self.assertEqual(signed(broken.mount_files()), -8)
        self.assertEqual(signed(broken.call('files_open', 1, self.path, 1)), -7)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *sys.argv[2:]])
