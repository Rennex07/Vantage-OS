"""Isolated EL0 processes on the linked kernel, under its real translation tables.

Usage: python tests/arm/processes.py path/to/fresh/kernel8.elf -v
User programs are assembled here and loaded by process_create. The CPU model
enforces the kernel's page tables, permissions and privilege levels; exception
entry and four CPUs are modeled (see cpu.py). Not Pi hardware validation.
"""
import struct
import sys
import unittest

from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0 as X0

from a64 import Program, NE
from cpu import (CpuMachine, image, signed, PROGRAM_BASE, USER_BASE, USER_END, PAGE, SCRATCH, STATUS,
                 FAULT_DATA, FAULT_INSTRUCTION, FAULT_UNDEFINED, FAULT_ADDRESS, OK, INVALID, PENDING)
from memory_management import Image

DATA = USER_BASE + PAGE           # Writable data of the single-text-page test images.
YIELD, EXIT, TASK_ID = 17, 18, 16
STACK_BYTES, STACK_SPAN = 16384, 0x10000
MMIO_UART = 0xfe201000


def exits(status):
    return Program(PROGRAM_BASE).syscall(EXIT, status)


def marker_program(rounds=12):
    """Exit with the word at DATA after checking it survives every yield."""
    p = Program(PROGRAM_BASE)
    p.mov(19, DATA).ldr(20, 19).mov(21, 0)
    p.label('loop').add_reg(22, 20, 21).str(22, 19, 8)
    p.mov(8, YIELD).svc()
    p.ldr(23, 19, 8).sub_reg(23, 23, 22).cbnz(23, 'fail')
    p.ldr(24, 19).sub_reg(24, 24, 20).cbnz(24, 'fail')
    p.add(21, 21, 1).cmp(21, rounds).b_cond(NE, 'loop')
    p.mov_reg(0, 20).mov(8, EXIT).svc()
    p.label('fail').syscall(EXIT, 99)
    return p


def marker_image(marker, bss=0):
    return image(marker_program().code(), data=struct.pack('<QQ', marker, 0), bss=bss)


class ProcessMachine(CpuMachine):
    def __init__(self, image):
        super().__init__(image)
        self.console = bytearray()
        self.device_calls = []
        self.keys = []
        for name in ('fb_wait_frame', 'canvas_present', 'pi4_drivers_poll', 'canvas_clear', 'canvas_set_color'):
            self.stub(name, lambda *a, name=name: self.device_calls.append(name))
        self.stub('uart_write_bytes', self.uart)
        self.stub('key_read', self.key)
        self.stub('touch_read', lambda *a: self.device_calls.append('touch_read'))
        self.stub('canvas_text_row', lambda *a: self.device_calls.append('canvas_text_row') or 1)
        self.stub('canvas_button', lambda *a: self.device_calls.append('canvas_button') or 1)
        self.stub('canvas_fill_circle', lambda *a: self.device_calls.append('canvas_fill_circle'))
        self.stub('canvas_text_columns', result=60)
        self.stub('canvas_text_rows', result=50)
        self.stub('time_us', result=1234)
        self.boot()
        self.baseline = self.free_pages()

    def uart(self, pointer, length, *_):
        self.console.extend(self.uc.mem_read(pointer, length))

    def key(self, pointer, *_):
        self.device_calls.append('key_read')
        event = self.keys.pop(0) if self.keys else (0, 0)
        self.uc.mem_write(pointer, struct.pack('<2I', *event))
        return bool(event[0])

    def run_all(self, **kwargs):
        self.start('task_run')
        return self.run(**kwargs)

    def info(self, process):
        if self.call('process_info', process, STATUS) != OK:
            return None
        names = ('id', 'state', 'status', 'threads', 'live', 'asid', 'pad', 'pages')
        return dict(zip(names, struct.unpack('<QIiIIIIQ', self.uc.mem_read(STATUS, 40))))


class ProcessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = Image(sys.argv[1])
        cls.m = ProcessMachine(cls.image)

    def setUp(self):
        m = self.m
        del m.entries[:], m.syscalls[:], m.maintenance[:], m.device_calls[:], m.console[:]

    def tearDown(self):
        # Every test must return all process memory to the page allocator.
        self.assertEqual(self.m.free_pages(), self.m.baseline)

    def finish(self, process, status):
        self.assertEqual(self.m.reap(process), (OK, status))

    def test_two_processes_keep_different_data_at_the_same_address(self):
        m = self.m
        for affinities in ((1, 1), (2, 4), (15, 15)):
            a = m.create_process(marker_image(0x11), affinities[0])
            b = m.create_process(marker_image(0x22), affinities[1])
            self.assertTrue(a and b)
            first, second = m.info(a), m.info(b)
            self.assertNotEqual(first['asid'], second['asid'])
            pa, _ = m.translate(m.process_root(a), DATA)
            pb, _ = m.translate(m.process_root(b), DATA)
            self.assertNotEqual(pa, pb)
            self.assertEqual((m.user_read(a, DATA, 8), m.user_read(b, DATA, 8)),
                             (struct.pack('<Q', 0x11), struct.pack('<Q', 0x22)))
            self.assertEqual(m.run_all(), OK)
            # Each wrote its own running value at DATA + 8 between the other's turns.
            self.assertEqual(m.user_read(a, DATA + 8, 8), struct.pack('<Q', 0x11 + 11))
            self.assertEqual(m.user_read(b, DATA + 8, 8), struct.pack('<Q', 0x22 + 11))
            cores = {asid: {core for core, caller, *_ in m.syscalls if caller == asid}
                     for asid in (first['asid'], second['asid'])}
            if affinities == (2, 4):
                self.assertEqual((cores[first['asid']], cores[second['asid']]), ({1}, {2}))
            self.finish(a, 0x11)
            self.finish(b, 0x22)

    def test_kernel_tables_are_privileged_only_and_text_is_read_only(self):
        m = self.m
        process = m.create_process(marker_image(1))
        root = m.process_root(process)
        kernel = struct.unpack('<4Q', m.uc.mem_read(self.image.symbols['l1'], 32))
        self.assertEqual(struct.unpack('<4Q', m.uc.mem_read(root & 0xfffffffff000, 32)), kernel)
        entries = struct.unpack('<2048Q', m.uc.mem_read(self.image.symbols['l2'], 2048 * 8))
        mapped = [entry for entry in entries if entry & 1]
        self.assertGreater(len(mapped), 64)
        for entry in mapped:
            self.assertEqual(entry & (1 << 6), 0, 'kernel block accessible from EL0')
            self.assertTrue(entry & (1 << 54), 'kernel block executable from EL0')
            self.assertEqual(entry & (1 << 11), 0, 'kernel mappings must be global')
        self.assertEqual((entries[0] >> 6) & 3, 2)                 # Kernel text: EL1 read-only.
        self.assertEqual([i for i, entry in enumerate(entries) if entry & 1 and not entry & (1 << 53)], [0])
        # User pages: EL0, non-global, never privileged-executable, W^X.
        for address, writable, executable in ((USER_BASE, 0, 1), (DATA, 1, 0), (USER_END - PAGE, 1, 0)):
            _, entry = m.translate(root, address)
            self.assertEqual(entry & ((1 << 6) | (1 << 11) | (1 << 53)), (1 << 6) | (1 << 11) | (1 << 53))
            self.assertEqual((not entry & (1 << 7), not entry & (1 << 54)), (bool(writable), bool(executable)))
        self.assertIsNone(m.translate(root, USER_END - STACK_BYTES - PAGE)[0])
        self.assertEqual(m.call('process_kill', process, -5), OK)
        self.assertEqual(m.run_all(), OK)
        self.finish(process, -5)

    def offenders(self, cases, batch=7, **parts):
        """Run faulting programs, a few at a time, beside a well-behaved peer."""
        m = self.m
        for start in range(0, len(cases), batch):
            peer = m.create_process(marker_image(0x33), 1)
            created = []
            for name, build, expected in cases[start:start + batch]:
                p = Program(PROGRAM_BASE)
                build(p)
                p.syscall(EXIT, 0)   # Reached only if the forbidden operation succeeded.
                created.append((name, m.create_process(image(p.code(), data=b'\x5a' * 64, **parts), 1), expected))
            self.assertTrue(peer and all(process for _, process, _ in created))
            self.assertEqual(m.run_all(), OK)
            for name, process, expected in created:
                with self.subTest(name):
                    self.finish(process, expected)
            self.finish(peer, 0x33)

    def test_user_access_outside_the_process_terminates_only_the_offender(self):
        m = self.m
        victim = m.create_process(marker_image(0x44, bss=3 * PAGE))
        victim_page, _ = m.translate(m.process_root(victim), DATA)
        kernel_data = self.image.symbols['l2']   # The kernel's own translation tables.
        before = bytes(m.uc.mem_read(kernel_data, 64))
        text_before = bytes(m.uc.mem_read(0x80000, 64))
        cases = [
            ('read kernel data', lambda p: p.mov(1, kernel_data).ldr(2, 1), FAULT_DATA),
            ('write kernel data', lambda p: p.mov(1, kernel_data).str(1, 1), FAULT_DATA),
            ('read kernel text', lambda p: p.mov(1, 0x80000).ldr(2, 1), FAULT_DATA),
            ('write kernel text', lambda p: p.mov(1, 0x80000).str(1, 1), FAULT_DATA),
            ('read kernel stack', lambda p: p.mov(1, self.image.symbols['__kernel_stack_top'] - 8).ldr(2, 1), FAULT_DATA),
            ('read device registers', lambda p: p.mov(1, MMIO_UART).ldr(2, 1), FAULT_DATA),
            ('write interrupt controller', lambda p: p.mov(1, 0xff841000).str(1, 1), FAULT_DATA),
            ("read another process's page through the kernel map", lambda p: p.mov(1, victim_page).ldr(2, 1), FAULT_DATA),
            ("write another process's page through the kernel map", lambda p: p.mov(1, victim_page).str(1, 1), FAULT_DATA),
            ('read memory only the other process maps', lambda p: p.mov(1, DATA + 2 * PAGE).ldr(2, 1), FAULT_DATA),
            ('write own text', lambda p: p.mov(1, USER_BASE + 64).str(1, 1), FAULT_DATA),
            ('read below the stack', lambda p: p.mov(1, USER_END - STACK_BYTES - 8).ldr(2, 1), FAULT_DATA),
            ('read above user memory', lambda p: p.mov(1, USER_END).ldr(2, 1), FAULT_DATA),
            ('read null', lambda p: p.mov(1, 0).ldr(2, 1), FAULT_DATA),
            ('execute data', lambda p: p.mov(1, DATA).br(1), FAULT_INSTRUCTION),
            ('execute the stack', lambda p: p.mov(1, USER_END - 64).br(1), FAULT_INSTRUCTION),
            ('jump into the kernel', lambda p: p.mov(1, self.image.symbols['handle_sys_call']).br(1), FAULT_INSTRUCTION),
            ('return from the entry point', lambda p: p.ret(), FAULT_INSTRUCTION),
            ('undefined instruction', lambda p: p.udf(), FAULT_UNDEFINED),
            ('read a translation register', lambda p: p.mrs(1, (3, 0, 2, 0, 0)), FAULT_UNDEFINED),
            ('mask interrupts', lambda p: p.emit(0xd50342df), FAULT_UNDEFINED),      # msr daifset, #2
            ('stop the preemption timer', lambda p: p.mov(1, 2).msr((3, 3, 14, 3, 1), 1), FAULT_UNDEFINED),
            ('move the preemption deadline', lambda p: p.mov(1, 0x7fffffff).msr((3, 3, 14, 3, 0), 1), FAULT_UNDEFINED),
            ('exception return', lambda p: p.emit(0xd69f03e0), FAULT_UNDEFINED),
        ]
        self.offenders(cases, batch=6)
        self.offenders([('write constants', lambda p: p.mov(1, USER_BASE + PAGE).str(1, 1), FAULT_DATA)],
                       rodata=b'constant')
        self.assertEqual(bytes(m.uc.mem_read(kernel_data, 64)), before)
        self.assertEqual(bytes(m.uc.mem_read(0x80000, 64)), text_before)
        # The other process ran beside the first offender and was never disturbed.
        self.assertEqual(m.user_read(victim, DATA, 16), struct.pack('<QQ', 0x44, 0x44 + 11))
        self.finish(victim, 0x44)

    def test_forbidden_syscall_buffers_terminate_the_caller_before_any_effect(self):
        m = self.m
        tables = self.image.symbols['l1']
        before = bytes(m.uc.mem_read(tables, PAGE))
        outputs, inputs = (1, 6, 8, 33, 37), (4, 7, 10, 11, 32, 34, 35, 38)
        cases = []
        for call in outputs + inputs:
            # The last bytes of a mapping: every payload here crosses its end.
            tail = 4 if call == 6 else 8
            pointers = [0, tables, 0x80000, self.image.symbols['tasks'], USER_BASE - 8,
                        DATA + PAGE - tail, DATA + PAGE, USER_END - tail, USER_END, 0xfffffffffffffff0]
            # Results must not be written to the read-only program text.
            for pointer in pointers + ([USER_BASE + 64] if call in outputs else []):
                cases.append((f'call {call} with {pointer:#x}',
                              lambda p, call=call, pointer=pointer: p.syscall(call, pointer), FAULT_ADDRESS))
        self.offenders(cases)
        self.assertEqual(m.device_calls, [])
        self.assertEqual(m.console, b'')
        self.assertEqual(bytes(m.uc.mem_read(tables, PAGE)), before)

    def test_valid_and_misaligned_syscall_buffers(self):
        m = self.m
        m.keys.append((1, ord('k')))
        p = Program(PROGRAM_BASE)
        p.mov(19, DATA)
        p.syscall(10, DATA).mov_reg(20, 0)            # Console write from user data.
        p.syscall(6, DATA + 0x100).mov_reg(21, 0)     # Key event copied out.
        p.syscall(6, DATA + 0x101).mov_reg(22, 0)     # Misaligned: rejected, not fatal.
        p.syscall(8, DATA + 0x204).mov_reg(23, 0)
        p.syscall(8, DATA + 0x200).mov_reg(24, 0)     # Console info copied out.
        p.syscall(12).mov_reg(25, 0)
        p.syscall(99, DATA).mov_reg(26, 0)            # Unknown call.
        p.str(20, 19, 0x300).str(21, 19, 0x308).str(22, 19, 0x310).str(23, 19, 0x318)
        p.str(24, 19, 0x320).str(25, 19, 0x328).str(26, 19, 0x330)
        p.syscall(EXIT, 0)
        text = struct.pack('<I', 5) + b'hello'
        process = m.create_process(image(p.code(), data=text, bss=PAGE - len(text)))
        self.assertEqual(m.run_all(), OK)
        self.assertEqual(m.console, b'hello')
        self.assertEqual(struct.unpack('<7Q', m.user_read(process, DATA + 0x300, 56)), (5, 1, 0, 0, 1, 1234, 0))
        self.assertEqual(struct.unpack('<2I', m.user_read(process, DATA + 0x100, 8)), (1, ord('k')))
        columns, rows, kind = struct.unpack('<3I', m.user_read(process, DATA + 0x200, 12))
        self.assertEqual((columns, rows, kind), (60, 50, 1))
        self.assertEqual(m.device_calls.count('key_read'), 1)
        self.finish(process, 0)

    def test_lifecycle_reclaims_memory_and_flushes_before_freeing(self):
        m = self.m
        self.assertEqual(m.reap(0)[0], INVALID)
        frees = []
        hook = m.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, _: frees.append(len(m.maintenance)),
                             begin=self.image.symbols['mm_free_pages'], end=self.image.symbols['mm_free_pages'])
        process = m.create_process(image(exits(42).code(), data=b'x' * 9, bss=3 * PAGE))
        info = m.info(process)
        self.assertEqual((info['state'], info['threads'], info['live']), (2, 1, 1))
        self.assertEqual(info['pages'], m.baseline - m.free_pages())
        self.assertEqual(info['pages'], 4 + 1 + 4 + STACK_BYTES // PAGE)
        self.assertIn((0, 'ic', 'ialluis', None), m.maintenance)
        self.assertEqual(m.reap(process)[0], PENDING)
        self.assertEqual(frees, [])
        self.assertEqual(m.run_all(), OK)
        self.assertEqual(m.info(process)['state'], 3)
        self.finish(process, 42)
        flush = m.maintenance.index((0, 'tlbi', 'aside1is', info['asid'] << 48))
        self.assertEqual(len(frees), info['pages'])
        self.assertGreater(min(frees), flush, 'pages were freed before the ASID was flushed on every core')
        self.assertIsNone(m.info(process))
        self.assertEqual(m.reap(process)[0], INVALID)
        self.assertEqual(m.call('process_kill', process, 1), INVALID)
        # The freed pages and the slot are reused by a different program.
        again = m.create_process(image(exits(43).code()))
        self.assertNotEqual(again, process)
        self.assertEqual(again & 63, process & 63)
        self.assertEqual(m.run_all(), OK)
        self.finish(again, 43)
        m.uc.hook_del(hook)

    def test_capacity_bad_images_and_wrong_callers(self):
        m = self.m
        good = image(exits(1).code())
        processes = [m.create_process(good) for _ in range(8)]
        self.assertTrue(all(processes))
        self.assertEqual(len({m.info(p)['asid'] for p in processes}), 8)
        self.assertEqual(m.create_process(good), 0)
        self.assertEqual(m.run_all(), OK)
        for process in processes:
            self.finish(process, 1)
        for mutate in (lambda b: b.__setitem__(0, b[0] ^ 1),                       # Magic.
                       lambda b: struct.pack_into('<Q', b, 8, USER_BASE + PAGE),   # Entry past the text.
                       lambda b: struct.pack_into('<Q', b, 8, PROGRAM_BASE + 2),
                       lambda b: struct.pack_into('<Q', b, 16, PAGE + 8),
                       lambda b: struct.pack_into('<Q', b, 40, 1 << 30)):
            blob = bytearray(good)
            mutate(blob)
            self.assertEqual(m.create_process(bytes(blob)), 0)
        self.assertEqual(m.create_process(good[:-1]), 0)
        self.assertEqual(m.create_process(good, 0), 0)
        self.assertEqual(m.create_process(good, 16), 0)
        # Only core 0 owns the page allocator.
        m.select(2)
        self.assertEqual(m.create_process(good, 4), 0)
        m.select(0)

    def test_threads_share_memory_on_separate_stacks_and_exit_together(self):
        m = self.m
        p = Program(PROGRAM_BASE)
        p.mov(19, DATA).mov_from_sp(1).str(1, 19, 32)
        p.label('wait').mov(8, YIELD).svc().ldr(2, 19, 16).cbz(2, 'wait')
        p.syscall(EXIT, 3)
        p.label('thread').mov(19, DATA).str(0, 19, 16).mov_from_sp(1).str(1, 19, 24)
        p.label('spin').mov(8, YIELD).svc().b('spin')
        process = m.create_process(image(p.code(), bss=PAGE), 1)
        thread = m.call('process_thread', process, p.address('thread'), 0xabc, 4)
        self.assertTrue(thread)
        self.assertEqual(m.call('process_thread', process, DATA, 0, 4), 0)       # Not executable.
        self.assertEqual(m.call('task_reap', thread, 0), INVALID)               # Owned by the process.
        self.assertEqual(m.info(process)['threads'], 2)
        self.assertEqual(m.run_all(), OK)
        shared, thread_sp, main_sp = struct.unpack('<3Q', m.user_read(process, DATA + 16, 24))
        self.assertEqual((shared, main_sp, thread_sp), (0xabc, USER_END, USER_END - STACK_SPAN))
        asid = m.info(process)['asid']
        self.assertEqual({core for core, caller, *_ in m.syscalls if caller == asid}, {0, 2})
        self.finish(process, 3)

    def test_yielding_process_migrates_with_its_registers(self):
        m = self.m
        p = Program(PROGRAM_BASE)
        for i in range(19, 29):
            p.mov(i, 0x7000000000000000 + i)
        for i in range(8, 16):
            p.mov(1, 0x4000000000000000 + i).fmov_to_d(i, 1)
        p.mov(9, 0)
        p.label('loop').mov(8, YIELD).svc().sub(2, 0, 1).cbnz(2, 'fail')
        for i in range(19, 29):
            p.mov(1, 0x7000000000000000 + i).sub_reg(1, 1, i).cbnz(1, 'fail')
        for i in range(8, 16):
            p.fmov_from_d(1, i).mov(2, 0x4000000000000000 + i).sub_reg(1, 1, 2).cbnz(1, 'fail')
        p.add(9, 9, 1).cmp(9, 24).b_cond(NE, 'loop')
        p.syscall(TASK_ID).mov(8, EXIT).svc()
        p.label('fail').syscall(EXIT, 99)
        movable = m.create_process(image(p.code()), 15)
        others = [m.create_process(marker_image(0x50 + i), 15) for i in range(5)]
        asid = m.info(movable)['asid']
        self.assertEqual(m.run_all(quantum=211), OK)
        self.assertGreater(len({core for core, caller, *_ in m.syscalls if caller == asid}), 1)
        result, status = m.reap(movable)
        self.assertEqual(result, OK)
        self.assertNotIn(status, (0, 99))   # Low bits of its task ID.
        for i, process in enumerate(others):
            self.finish(process, 0x50 + i)

    def test_frame_wait_yields_to_other_tasks_even_without_a_redraw(self):
        m = self.m
        waiter = m.create_process(image(Program(PROGRAM_BASE).syscall(9).syscall(EXIT, 1).code()))
        peer = m.create_process(image(Program(PROGRAM_BASE).syscall(YIELD).syscall(YIELD).syscall(EXIT, 2).code()))
        first, second = m.info(waiter)['asid'], m.info(peer)['asid']
        self.assertEqual(m.run_all(), OK)
        calls = [(caller, number) for _, caller, number, _ in m.syscalls]
        # The peer runs between the frame wait and the waiter's next call.
        self.assertEqual(calls[calls.index((first, 9)) + 1][0], second)
        self.assertLess(calls.index((first, 9)), calls.index((first, EXIT)))
        self.finish(waiter, 1)
        self.finish(peer, 2)

    def test_kernel_tasks_run_without_a_user_address_space(self):
        m = self.m
        roots = []
        hook = m.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, _: roots.append(uc.reg_read(X0)),
                             begin=self.image.symbols['mmu_install_space'],
                             end=self.image.symbols['mmu_install_space'])
        self.assertEqual(m.call('task_self_test'), 1)
        self.assertEqual(set(roots), {0})   # Kernel-only tasks: never a process root.
        del roots[:]
        process = m.create_process(marker_image(0x61), 1)
        self.assertEqual(m.run_all(), OK)
        m.uc.hook_del(hook)
        self.assertEqual(set(roots), {0, m.process_root(process)})
        self.finish(process, 0x61)


if __name__ == '__main__':
    if len(sys.argv) < 2: raise SystemExit(__doc__)
    unittest.main(argv=[sys.argv[0], *sys.argv[2:]])
