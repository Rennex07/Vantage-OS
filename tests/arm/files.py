"""Linked persistent-file and EL0 file-syscall regressions with modeled exceptions.

Usage: python tests/arm/files.py path/to/fresh/kernel8.elf -v
No SD operations or physical Pi hardware are exercised.
"""
import struct
import sys
import unittest

from memory_management import Image
from tasks import signed, OK
from file_storage import FileMachine

OPEN, READ, WRITE, SEEK, CLOSE, LIST, REMOVE = range(32, 39)
BAD, NOT_FOUND, NO_SPACE, ACCESS, BAD_HANDLE = range(-1, -6, -1)


class FileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image = Image(sys.argv[1])

    def setUp(self):
        self.m = FileMachine(self.image)
        self.m.start()
        self.assertEqual(self.m.mount_files(), 0)
        self.path = self.image.symbols['__user_start'] + 0x10000
        self.input, self.output, self.entry = self.path + 64, self.path + 128, self.path + 256
        self.request = self.path + 512
        self.m.uc.mem_write(self.path, b'/ram/hello\0')
        self.m.uc.mem_write(self.input, b'hello')

    def test_el0_file_wrappers_round_trip(self):
        bottom = self.image.symbols['user_stack']
        self.m.uc.mem_write(bottom, b'\xa7' * 64)
        program = None
        handle = lambda: program['results'][0]
        program = self.m.create([
            ('sys_file_open', (self.path, 7)),
            ('sys_file_write', (handle, self.input, 5)),
            ('sys_file_seek', (handle, 0)),
            ('sys_file_read', (handle, self.output, 16)),
            ('sys_file_list', (self.entry,)),
            ('sys_file_close', (handle,)),
            ('sys_file_remove', (self.path,)),
            ('sys_task_exit', (0,))], user=True)
        self.assertEqual(self.m.call('task_run'), OK)
        self.assertGreater(program['results'][0], 0)
        self.assertEqual(program['results'][1:], [5, 0, 5, 1, 0, 0])
        self.assertEqual(self.m.uc.mem_read(self.output, 5), b'hello')
        cursor, size, path = struct.unpack('<II48s', self.m.uc.mem_read(self.entry, 56))
        self.assertEqual((cursor, size, path.rstrip(b'\0')), (1, 5, b'/ram/hello'))
        self.assertEqual(self.m.uc.mem_read(bottom, 64), b'\xa7' * 64)

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

    def test_terminal_editor_saves_from_el0_through_file_syscalls(self):
        terminal = self.image.symbols['terminal']
        bottom = self.image.symbols['user_stack']
        self.m.uc.mem_write(bottom, b'\xa7' * 64)
        steps = [('terminal_init', (terminal, 60, 33, 0, 0, 0))]

        def event(kind, character):
            def prepare():
                self.m.uc.mem_write(self.entry, struct.pack('<II', kind, character))
                return self.entry
            return prepare

        for line in ('edit hello', 'written by terminal'):
            for character in line:
                steps.append(('terminal_key', (terminal, event(1, ord(character)))))
            steps.append(('terminal_key', (terminal, event(2, 0))))
        steps += [('terminal_editor_action', (terminal, 0)),
                  ('terminal_editor_action', (terminal, 1))]
        open_index = len(steps)
        program = None
        handle = lambda: program['results'][open_index]
        steps += [('sys_file_open', (self.path, 1)),
                  ('sys_file_read', (handle, self.output, 64)),
                  ('sys_file_close', (handle,)), ('sys_task_exit', (0,))]
        program = self.m.create(steps, user=True)
        self.assertEqual(self.m.call('task_run'), OK)
        self.assertGreater(program['results'][open_index], 0)
        self.assertEqual(program['results'][open_index + 1:], [20, 0])
        self.assertEqual(self.m.uc.mem_read(self.output, 20), b'written by terminal\n')
        self.assertEqual(self.m.uc.mem_read(bottom, 64), b'\xa7' * 64)

    def test_task_ownership_and_automatic_exit_cleanup(self):
        program = None
        program = self.m.create([
            ('sys_file_open', (self.path, 7)),
            ('sys_file_write', (lambda: program['results'][0], self.input, 5)),
            ('sys_task_yield', ()), ('sys_task_exit', (0,))], user=True)

        def stolen_request():
            self.m.uc.mem_write(self.request, struct.pack('<QII256s', program['results'][0], 5, 0, b''))
            return self.request

        def open_request():
            self.m.uc.mem_write(self.request, struct.pack('<II48s', 1, 0, b'/ram/hello'))
            return self.request

        peer = self.m.create([
            ('handle_sys_call', (READ, stolen_request)),
            ('task_yield', ()),
            ('handle_sys_call', (OPEN, open_request))])
        self.assertEqual(self.m.call('task_run'), OK)
        self.assertEqual(peer['results'][:2], [BAD_HANDLE, OK])
        self.assertGreater(peer['results'][2], 0)
        # Both tasks exited without explicit close; no live handles may block removal.
        self.assertEqual(self.m.call('files_remove', 99, self.path), 0)

    def test_syscall_rejects_kernel_partial_unaligned_and_wrapping_buffers(self):
        request = struct.pack('<II48s', 7, 0, b'/ram/hello')
        self.m.uc.mem_write(self.request, request)
        for address in (0, self.request + 1, self.image.symbols['__kernel_stack_bottom'],
                        self.image.symbols['__user_end'] - 4, 0xfffffffffffffff0):
            self.assertEqual(signed(self.m.call('handle_file_call', OPEN, address, 1)), BAD)
        self.assertEqual(signed(self.m.call('files_open', 1, self.path, 1)), NOT_FOUND)
        handle = self.m.call('handle_file_call', OPEN, self.request, 1)
        self.assertGreater(handle, 0)
        self.m.uc.mem_write(self.request, struct.pack('<QII256s', handle, 3, 0, b'abc'))
        self.assertEqual(self.m.call('handle_file_call', WRITE, self.request, 1), 3)
        self.assertEqual(self.m.call('files_seek', 1, handle, 0), 0)
        for length, reserved in ((257, 0), (3, 1)):
            self.m.uc.mem_write(self.request, struct.pack('<QII256s', handle, length, reserved, b''))
            self.assertEqual(signed(self.m.call('handle_file_call', READ, self.request, 1)), BAD)
        self.m.uc.mem_write(self.request, struct.pack('<QII256s', handle, 256, 0, b'\xcc' * 256))
        self.assertEqual(self.m.call('handle_file_call', READ, self.request, 1), 3)
        _, length, reserved, data = struct.unpack('<QII256s', self.m.uc.mem_read(self.request, 272))
        self.assertEqual((length, reserved, data), (3, 0, b'abc' + b'\0' * 253))

    def test_bad_paths_flags_and_unavailable_owner_do_not_create_files(self):
        for path, flags, reserved in ((b'/ram/../evil', 7, 0), (b'/ram/' + b'x' * 40, 7, 0),
                                      (b'/ram/hello', 0x100, 0), (b'/ram/hello', 7, 1)):
            self.m.uc.mem_write(self.request, struct.pack('<II48s', flags, reserved, path))
            self.assertEqual(signed(self.m.call('handle_file_call', OPEN, self.request, 1)), BAD)
        self.assertEqual(signed(self.m.call('files_open', 1, self.path, 1)), NOT_FOUND)
        self.assertEqual(signed(self.m.call('handle_sys_call', OPEN, self.request)), -7)

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
