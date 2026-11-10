"""Timer preemption of EL0 processes and kernel regions on the linked kernel.

Usage: python tests/arm/preemption.py path/to/fresh/kernel8.elf -v
The kernel arms the modeled per-core virtual timer; when a slice has run out
the model raises the interrupt at whatever instruction the CPU has reached,
if that context has IRQs unmasked. Vectors, trap frames, the scheduler tick,
address-space switches and exception returns execute natively (see cpu.py).
Slices are counted in instructions: this is not timing or Pi validation.
"""
import struct
import sys
import unittest

from a64 import Program, FPCR, FPSR, NZCV
from cpu import (CpuMachine, image, PROGRAM_BASE, USER_BASE, USER_END, PAGE, TIMER_IRQ, OK)
from interrupts import x_pattern, v_pattern
from memory_management import Image
from processes import ProcessMachine, marker_image, DATA, EXIT

EXITED = 3
SLICE_TICKS = 540000   # 10 ms at the modeled 54 MHz counter.


def hog(offset=0):
    """Increment a counter in user memory forever, never entering the kernel."""
    p = Program(PROGRAM_BASE)
    p.mov(19, DATA + offset)
    p.label('loop').ldr(1, 19).add(1, 1, 1).str(1, 19).b('loop')
    return p


def countdown():
    """Count the word at DATA down to zero without a system call, then exit
    with the word at DATA + 8."""
    p = Program(PROGRAM_BASE)
    p.mov(19, DATA)
    p.label('loop').ldr(1, 19).sub(1, 1, 1).str(1, 19).cbnz(1, 'loop')
    p.ldr(0, 19, 8).mov(8, EXIT).svc()
    return p


def countdown_image(count, marker):
    return image(countdown().code(), data=struct.pack('<QQ', count, marker))


def register_holder(iterations):
    """Hold a pattern in every register across a long loop that only uses x0
    and its stack, then store the registers below the stack top and exit."""
    p = Program(PROGRAM_BASE)
    p.mov_from_sp(0).sub(0, 0, 1024).mov_to_sp(0)
    p.mov(0, iterations).str(0, 31)
    for i in range(32):
        value = v_pattern(i)
        p.mov(0, value & 0xffffffffffffffff).fmov_to_d(i, 0)
        p.mov(0, value >> 64).ins_high(i, 0)
    p.mov(0, 0x00c00000).msr(FPCR, 0)
    p.mov(0, 0x0000001b).msr(FPSR, 0)
    for i in range(1, 31):
        p.mov(i, x_pattern(i))
    p.cmp(5, 0)
    p.label('loop').ldr(0, 31).sub(0, 0, 1).str(0, 31).cbnz(0, 'loop')
    for i in range(1, 31):
        p.str(i, 31, 8 * i)
    p.mrs(0, NZCV).str(0, 31, 0x100).mrs(0, FPCR).str(0, 31, 0x108).mrs(0, FPSR).str(0, 31, 0x110)
    for i in range(32):
        p.fmov_from_d(0, i).str(0, 31, 0x200 + 16 * i)
        p.umov_high(0, i).str(0, 31, 0x208 + 16 * i)
    p.syscall(EXIT, 0)
    return p


class PreemptionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = Image(sys.argv[1])
        cls.m = ProcessMachine(cls.image)

    def setUp(self):
        m = self.m
        m.slice_instructions = 400
        del m.entries[:], m.interrupted[:], m.completed[:], m.syscalls[:], m.expired_syscalls[:]
        for cpu in m.cores:
            del cpu.timer_log[:]

    def tearDown(self):
        self.assertEqual(self.m.free_pages(), self.m.baseline)
        self.assertFalse(any(cpu.armed for cpu in self.m.cores), 'a timer was left running on an idle core')

    def state(self, process):
        return self.m.process_field(process, 16, 4)

    def counter(self, process, offset=0):
        return struct.unpack('<Q', self.m.user_read(process, DATA + offset, 8))[0]

    def check_interrupts(self, origins={0}):
        m = self.m
        irqs = [(core, origin) for core, kind, origin, _ in m.entries if kind == 'irq']
        self.assertTrue(irqs)
        self.assertEqual({origin for _, origin in irqs}, origins)
        # Each interrupt was acknowledged and completed once, on the core that took it.
        self.assertEqual(sorted(core for core, _ in irqs), sorted(core for core, _ in m.completed))
        self.assertEqual({value for _, value in m.completed}, {TIMER_IRQ})
        return irqs

    def test_non_yielding_loop_cannot_keep_its_core_and_is_killed_within_a_slice(self):
        m = self.m
        looping = m.create_process(image(hog().code(), bss=PAGE), 1)
        peer = m.create_process(marker_image(0x21), 1)
        m.start('task_run')
        # The peer shares core 0: it only ever runs if the loop is preempted.
        self.assertIsNone(m.run(until=lambda: self.state(peer) == EXITED and m.lock_free()))
        self.assertGreater(self.counter(looping), 100)
        self.check_interrupts()
        text = range(USER_BASE, USER_BASE + PAGE)
        self.assertTrue(all(pc in text for *_, pc in m.interrupted))
        self.assertEqual({core for core, *_ in m.interrupted}, {0})
        self.assertEqual({cpu.index for cpu in m.cores if cpu.timer_log}, {0})
        log = m.cores[0].timer_log
        self.assertEqual({value for kind, value in log if kind == 'interval'}, {SLICE_TICKS})
        # One slice per dispatch, each stopped again by the dispatcher or the tick.
        self.assertGreaterEqual([kind for kind, _ in log].count('stop'), [kind for kind, _ in log].count('start'))
        before = self.counter(looping)
        self.assertEqual(m.interject('process_kill', looping, -9), OK)
        self.assertEqual(m.run(), OK)
        # It executed at most the rest of its slice (four instructions per count).
        self.assertLessEqual((self.counter(looping) - before) * 4, m.slice_instructions + 400)
        self.assertEqual(m.reap(looping), (OK, -9))
        self.assertEqual(m.reap(peer), (OK, 0x21))

    def test_registers_of_a_preempted_process_survive_migration(self):
        m = self.m
        m.slice_instructions = 173
        holder = m.create_process(image(register_holder(2500).code()), 15)
        asid = m.info(holder)['asid']
        others = [m.create_process(countdown_image(1500 + 300 * i, 0x30 + i), 15) for i in range(5)]
        self.assertEqual(m.run_all(quantum=131), OK)
        taken = [(core, pc) for core, caller, pc in m.interrupted if caller == asid]
        self.assertGreater(len(taken), 20)
        self.assertGreater(len({core for core, _ in taken}), 1, 'the preempted process never changed cores')
        self.assertGreater(len({pc for _, pc in taken}), 3, 'interrupts always hit the same instruction')
        self.check_interrupts()
        base = USER_END - 1024
        saved = struct.unpack('<32Q', m.user_read(holder, base, 256))
        self.assertEqual(saved[0], 0)
        for i in range(1, 31):
            self.assertEqual(saved[i], x_pattern(i), f'x{i}')
        flags, control, status = struct.unpack('<3Q', m.user_read(holder, base + 0x100, 24))
        self.assertEqual((flags, control, status), (0x20000000, 0x00c00000, 0x1b))
        vectors = struct.unpack('<64Q', m.user_read(holder, base + 0x200, 512))
        for i in range(32):
            self.assertEqual(vectors[2 * i] | (vectors[2 * i + 1] << 64), v_pattern(i), f'v{i}')
        self.assertEqual(m.reap(holder), (OK, 0))
        for i, process in enumerate(others):
            self.assertEqual(m.reap(process), (OK, 0x30 + i))

    def test_address_spaces_stay_separate_under_preemption_and_migration(self):
        m = self.m
        m.slice_instructions = 211
        counts = (1200, 1700, 900, 2100, 1500, 1000)
        processes = [m.create_process(countdown_image(count, 0x40 + i), 15) for i, count in enumerate(counts)]
        asids = [m.info(process)['asid'] for process in processes]
        pages = {m.translate(m.process_root(process), DATA)[0] for process in processes}
        self.assertEqual(len(pages), len(processes))   # Same address, six different pages.
        self.assertEqual(m.run_all(quantum=149), OK)
        for i, process in enumerate(processes):
            # Each counted exactly its own word down; nobody touched another's.
            self.assertEqual(struct.unpack('<QQ', m.user_read(process, DATA, 16)), (0, 0x40 + i))
            cores = {core for core, caller, _ in m.interrupted if caller == asids[i]}
            self.assertTrue(cores)
            self.assertEqual(m.reap(process), (OK, 0x40 + i))
        self.assertGreater(len({core for core, *_ in m.interrupted}), 1)
        self.check_interrupts()

    def test_tick_pending_during_a_system_call_is_taken_on_return_to_user(self):
        m = self.m
        m.slice_instructions = 97
        p = Program(PROGRAM_BASE)
        p.mov(19, 300)
        p.label('loop').mov(8, 12).svc().label('after').sub(19, 19, 1).cbnz(19, 'loop')
        p.syscall(EXIT, 5)
        process = m.create_process(image(p.code()), 1)
        peer = m.create_process(countdown_image(2000, 0x51), 1)
        self.assertEqual(m.run_all(), OK)
        # The kernel itself is never interrupted: the tick waits for the return.
        self.check_interrupts(origins={0})
        # Calls ran to completion while their slice had already expired. Where the model
        # samples the pending tick depends on its chunking, so only EL0 is asserted.
        self.assertIn(12, [number for _, number in m.expired_syscalls])
        self.assertTrue(all(pc in range(USER_BASE, USER_BASE + PAGE) for *_, pc in m.interrupted))
        self.assertEqual(m.reap(process), (OK, 5))
        self.assertEqual(m.reap(peer), (OK, 0x51))

    def test_kernel_tasks_are_preempted_only_inside_preemptible_regions(self):
        m = self.m
        m.slice_instructions = 60
        # Ordinary kernel tasks run masked: expired slices are never delivered.
        self.assertEqual(m.call('task_self_test'), 1)
        self.assertEqual(m.entries, [])
        m.start('task_multicore_self_test')
        self.assertEqual(m.run(), 1)
        self.assertEqual(m.entries, [])
        # The boot probe opens a preemptible region on every core.
        m.start('task_preemption_self_test')
        self.assertEqual(m.run(), 1)
        irqs = self.check_interrupts(origins={1})
        self.assertEqual({core for core, _ in irqs}, {0, 1, 2, 3})
        kernel_text = range(0x80000, 0x200000)
        self.assertTrue(all(pc in kernel_text and asid == 0 for _, asid, pc in m.interrupted))

    def test_threads_of_a_killed_process_stop_on_every_core(self):
        m = self.m
        p = hog()
        p.label('thread').mov(19, DATA + 8)
        p.label('again').ldr(1, 19).add(1, 1, 1).str(1, 19).b('again')
        process = m.create_process(image(p.code(), bss=PAGE), 2)
        self.assertTrue(m.call('process_thread', process, p.address('thread'), 0, 4))
        m.start('task_run')
        self.assertIsNone(m.run(until=lambda: self.counter(process) > 50 and self.counter(process, 8) > 50
                                and m.lock_free()))
        self.assertEqual(self.state(process), 2)
        self.assertEqual(m.interject('process_kill', process, -7), OK)
        self.assertEqual(m.run(), OK)
        self.assertEqual({core for core, *_ in m.interrupted}, {1, 2})
        self.assertEqual(m.reap(process), (OK, -7))


if __name__ == '__main__':
    if len(sys.argv) < 2: raise SystemExit(__doc__)
    unittest.main(argv=[sys.argv[0], *sys.argv[2:]])
