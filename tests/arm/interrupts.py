"""Linked trap-frame, GIC-400 and virtual-timer checks on the CPU model.

Usage: python tests/arm/interrupts.py path/to/fresh/kernel8.elf -v
The vectors, frame save/restore, interrupt service and exception returns execute
natively. Interrupt arrival, the GIC and the timer are modeled; this does not
validate Pi firmware handoff or real interrupt delivery.
"""
import struct
import sys
import unittest

from unicorn import UC_HOOK_CODE
from unicorn import arm64_const as regs

from a64 import Program, FPCR, FPSR, NE
from cpu import (CpuMachine, KERNEL_CODE, X, V, TIMER_IRQ, GICD_CTLR, GICD_ISENABLER0,
                 GICD_IPRIORITYR, GICC_CTLR, GICC_PMR, GICC_EOIR)
from memory_management import Image

FRAME_BYTES = 800
USER_STACK = 0x7b00000
EL0, EL1_IRQS_ON = 0, 0x345
CNTKCTL_EL1 = (14, 1, 3, 0, 0)
CALLER_SAVED_X = range(2, 19)


def x_pattern(i): return 0x1111000000000000 * (i % 15 + 1) + 0x0101 * i + 7
def v_pattern(i): return ((0xa000000000000000 + 0x1f1f * i) << 64) | (0x0b00000000000000 + 0x2e2e * i + 1)


def register_program(base=KERNEL_CODE):
    """Load every general and SIMD register, flags and FP control, then work."""
    p = Program(base)
    for i in range(32):
        value = v_pattern(i)
        p.mov(0, value & 0xffffffffffffffff).fmov_to_d(i, 0)
        p.mov(0, value >> 64).ins_high(i, 0)
    p.mov(0, 0x00c00000).msr(FPCR, 0)      # Round toward zero.
    p.mov(0, 0x0000001b).msr(FPSR, 0)      # Sticky exception flags.
    for i in range(1, 31):
        p.mov(i, x_pattern(i))
    p.mov(0, x_pattern(0))
    p.mov(3, 0).label('loop').add(3, 3, 1).add_reg(4, 4, 3).cmp(3, 6).b_cond(NE, 'loop')
    p.cmp(5, 0)                            # Leaves a known NZCV for the end.
    p.label('end').nop()
    return p


def read_frame(machine, pointer):
    words = struct.unpack('<100Q', machine.uc.mem_read(pointer, FRAME_BYTES))
    frame = {f'x{i}': words[i] for i in range(31)}
    frame.update(sp_el0=words[31], elr=words[32], spsr=words[33], fpcr=words[34], fpsr=words[35])
    frame.update({f'v{i}': words[36 + 2 * i] | (words[37 + 2 * i] << 64) for i in range(32)})
    return frame


def enter(machine, pc, spsr, sp_el0=0, x=None):
    """Leave EL1 through the kernel's own frame restore."""
    top = machine.stack_top()
    words = [0] * 100
    for index, value in (x or {}).items():
        words[index] = value
    words[31], words[32], words[33] = sp_el0, pc, spsr
    machine.uc.mem_write(top - FRAME_BYTES, struct.pack('<100Q', *words))
    machine.uc.reg_write(regs.UC_ARM64_REG_SP, top)
    machine.uc.reg_write(regs.UC_ARM64_REG_X0, top - FRAME_BYTES)
    machine.uc.reg_write(regs.UC_ARM64_REG_PC, machine.image.symbols['trap_return'])


def scramble(machine, keep=()):
    """Clobber everything the procedure call standard lets kernel C clobber."""
    uc = machine.uc
    for i in CALLER_SAVED_X:
        if i not in keep:
            uc.reg_write(X[i], 0xdead000000000000 | i)
    for i in range(32):
        low = uc.reg_read(V[i]) & 0xffffffffffffffff
        callee_saved_low = 8 <= i <= 15
        uc.reg_write(V[i], (0xbad0 << 64) | (low if callee_saved_low else 0xbad1))
    uc.reg_write(regs.UC_ARM64_REG_NZCV, 0xf0000000)
    uc.reg_write(regs.UC_ARM64_REG_FPSR, 0x08000001)


class InterruptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = Image(sys.argv[1])
        cls.program = register_program()

    def machine(self):
        m = CpuMachine(self.image)
        m.uc.mem_write(KERNEL_CODE, self.program.code())
        return m

    def branch_target(self, m, address):
        word = struct.unpack('<I', m.uc.mem_read(address, 4))[0]
        self.assertEqual(word >> 26, 5, 'vector entry must be an unconditional branch')
        offset = word & 0x3ffffff
        return address + 4 * (offset - (1 << 26) if offset >> 25 else offset)

    def test_vectors_route_interrupts_and_el0_calls_to_frame_handlers(self):
        m = self.machine()
        table = self.image.symbols['vector_table']
        self.assertEqual(table % 0x800, 0)
        targets = [self.branch_target(m, table + 0x80 * index) for index in range(16)]
        irq, sync = self.image.symbols['irq_exception'], self.image.symbols['el0_sync_exception']
        self.assertEqual(targets[5], irq)    # EL1 using SP_EL1
        self.assertEqual(targets[9], irq)    # EL0
        self.assertEqual(targets[8], sync)
        park = targets[0]
        self.assertEqual([t for i, t in enumerate(targets) if i not in (5, 8, 9)], [park] * 13)

    def test_each_core_enables_only_its_own_timer_interrupt(self):
        m = self.machine()
        for core in range(4):
            m.select(core)
            m.uc.reg_write(regs.UC_ARM64_REG_CP_REG, CNTKCTL_EL1 + (0x303,))
            self.assertEqual(m.call('irq_init_core'), 1)
            # User code gets no access to the timer registers.
            self.assertEqual(m.system(CNTKCTL_EL1), 0)
            writes = [(address, value) for c, address, value in m.gic_writes if c == core]
            self.assertEqual((GICD_CTLR, 1) in writes, core == 0)
            self.assertIn((GICD_ISENABLER0, 1 << TIMER_IRQ), writes)
            self.assertIn((GICD_IPRIORITYR + TIMER_IRQ, 0xa0), writes)
            self.assertIn((GICC_PMR, 0xf0), writes)
            self.assertIn((GICC_CTLR, 1), writes)
            self.assertEqual(len(writes), 5 if core == 0 else 4)
            self.assertEqual(m.cpu.timer_log, [('stop', 2)])
            self.assertFalse(m.cpu.armed)

    def test_unusable_interrupt_setup_is_reported(self):
        m = self.machine()
        m.gic_group0 = True   # Secure Group 0 state is invisible to the kernel.
        self.assertEqual(m.call('irq_init_core'), 0)
        m = self.machine()
        m.set_pstate(EL1_IRQS_ON)
        self.assertEqual(m.call('irq_init_core'), 0)
        self.assertEqual(m.gic_writes, [])

    def test_timer_interval_follows_the_counter_frequency(self):
        m = self.machine()
        for microseconds, ticks in ((10000, 540000), (1, 54), (0, 1), (0xffffffff, 0x7fffffff)):
            m.cpu.timer_log.clear()
            m.call('irq_timer_start', microseconds)
            self.assertEqual(m.cpu.timer_log, [('interval', ticks), ('start', 1)])
            self.assertTrue(m.cpu.armed)
        m.call('irq_timer_stop')
        self.assertEqual(m.cpu.timer_log[-1], ('stop', 2))
        self.assertFalse(m.cpu.armed)

    def test_service_completes_real_interrupts_and_ignores_spurious_ones(self):
        m = self.machine()
        self.assertEqual(m.call('irq_service'), 1023)
        self.assertEqual(m.completed, [])
        m.call('irq_timer_start', 10000)
        m.cpu.remaining = 0
        self.assertEqual(m.call('irq_service'), TIMER_IRQ)
        self.assertEqual(m.completed, [(0, TIMER_IRQ)])
        self.assertFalse(m.cpu.armed, 'the level-sensitive timer must be stopped before completion')
        m.cpu.forced_irq = 100
        self.assertEqual(m.call('irq_service'), 100)
        self.assertEqual(m.completed[-1], (0, 100))

    def reference(self, spsr):
        m = self.machine()
        enter(m, KERNEL_CODE, spsr, USER_STACK)
        self.assertTrue(m.advance(100000, stop=self.program.address('end')))
        return m.state()

    def interrupted_machine(self):
        m = self.machine()
        m.seen = []

        def at_handler(uc, address, size, _):
            m.seen.append(read_frame(m, uc.reg_read(regs.UC_ARM64_REG_X0)))
            m.seen.append(uc.reg_read(regs.UC_ARM64_REG_X0))

        def in_service(uc, address, size, _):
            scramble(m)

        m.uc.hook_add(UC_HOOK_CODE, at_handler, begin=self.image.symbols['trap_irq'],
                      end=self.image.symbols['trap_irq'])
        m.uc.hook_add(UC_HOOK_CODE, in_service, begin=self.image.symbols['irq_service'],
                      end=self.image.symbols['irq_service'])
        return m

    def interrupted_run(self, m, spsr, index, expected):
        del m.seen[:], m.entries[:], m.completed[:]
        m.set_pstate(0x3c5)
        enter(m, KERNEL_CODE, spsr, USER_STACK)
        self.assertTrue(m.advance(1000, stop=KERNEL_CODE))
        self.assertFalse(m.advance(index, stop=self.program.address('end')))
        before = m.state()
        user_sp = before['sp'] if spsr == EL0 else m.uc.reg_read(regs.UC_ARM64_REG_SP_EL0)
        m.cpu.forced_irq = TIMER_IRQ
        self.assertTrue(m.advance(100000, stop=self.program.address('end')))
        self.assertEqual(m.entries, [(0, 'irq', 0 if spsr == EL0 else 1, before['pc'])])
        self.assertEqual(m.completed, [(0, TIMER_IRQ)])
        frame, pointer = m.seen
        for name in [f'x{i}' for i in range(31)] + [f'v{i}' for i in range(32)] + ['fpcr', 'fpsr']:
            self.assertEqual(frame[name], before[name], f'{name} saved at instruction {index}')
        self.assertEqual((frame['elr'], frame['spsr'], frame['sp_el0']),
                         (before['pc'], before['pstate'], user_sp))
        kernel_sp = m.stack_top() if spsr == EL0 else before['sp']
        self.assertEqual(pointer, kernel_sp - FRAME_BYTES)
        self.assertEqual(m.state(), expected, f'state after interrupt at instruction {index}')
    def test_interrupt_at_every_el1_instruction_preserves_all_state(self):
        expected = self.reference(EL1_IRQS_ON)
        self.assertEqual((expected['x3'], expected['x4']), (6, x_pattern(4) + 21))
        self.assertEqual(expected['pstate'] & 0x3cf, EL1_IRQS_ON)
        m = self.interrupted_machine()
        for index in range(len(self.program.words) - 1):
            self.interrupted_run(m, EL1_IRQS_ON, index, expected)

    def test_interrupt_at_every_el0_instruction_preserves_all_state(self):
        expected = self.reference(EL0)
        self.assertEqual((expected['sp'], expected['pstate'] & 0x3cf), (USER_STACK, 0))
        self.assertEqual(expected['v31'], v_pattern(31))
        self.assertEqual((expected['fpcr'], expected['fpsr']), (0x00c00000, 0x1b))
        m = self.interrupted_machine()
        for index in range(len(self.program.words) - 1):
            self.interrupted_run(m, EL0, index, expected)

    def test_masked_el1_code_is_not_interrupted(self):
        m = self.machine()
        enter(m, KERNEL_CODE, 0x3c5)
        m.cpu.forced_irq = TIMER_IRQ
        self.assertTrue(m.advance(100000, stop=self.program.address('end')))
        self.assertEqual((m.entries, m.completed), ([], []))

    def test_el0_system_call_round_trips_through_the_complete_frame(self):
        call = Program(KERNEL_CODE + 0x4000)
        call.mov(8, 16).svc().label('after').nop()   # SYS_TASK_ID outside the scheduler: 0.
        m = self.machine()
        m.uc.mem_write(call.base, call.code())
        seen = []

        def at_handler(uc, address, size, _):
            seen.append(read_frame(m, uc.reg_read(regs.UC_ARM64_REG_X0)))

        def in_dispatch(uc, address, size, _):
            seen.append((uc.reg_read(X[0]), uc.reg_read(X[1])))
            scramble(m)

        for name, hook in (('trap_sync_el0', at_handler), ('handle_sys_call', in_dispatch)):
            m.uc.hook_add(UC_HOOK_CODE, hook, begin=self.image.symbols[name], end=self.image.symbols[name])
        enter(m, KERNEL_CODE, EL0, USER_STACK)
        self.assertTrue(m.advance(100000, stop=self.program.address('end')))
        before = m.state()
        m.uc.reg_write(regs.UC_ARM64_REG_PC, call.base)
        self.assertTrue(m.advance(100000, stop=call.address('after')))
        frame, arguments = seen
        self.assertEqual(arguments, (16, before['x0']))
        self.assertEqual((frame['elr'], frame['sp_el0'], frame['spsr']),
                         (call.address('after'), USER_STACK, before['pstate']))
        after = m.state()
        self.assertEqual(m.entries, [(0, 'sync', 0, call.address('after'))])
        expected = dict(before, x0=0, x8=16, pc=call.address('after'))
        self.assertEqual(after, expected)


if __name__ == '__main__':
    if len(sys.argv) < 2: raise SystemExit(__doc__)
    unittest.main(argv=[sys.argv[0], *sys.argv[2:]])
