"""Execute the linked scheduler and context switch for kernel tasks on mocked ARM.

Usage: python tests/arm/tasks.py path/to/fresh/kernel8.elf -v
Uses the memory harness dependencies. Probe bodies are scripted kernel tasks;
scheduler C and stack switching execute. EL0 processes, system calls from user
mode, interrupts and address-space isolation are tested on the CPU model in
processes.py, preemption.py and interrupts.py. This is not a Pi test.
"""
import struct
import sys
import unittest

from unicorn import UC_HOOK_CODE
from unicorn import arm64_const as regs

from memory_management import Image, Machine, RETURN, STATS

PROBE = 0x7000000
RESUME = PROBE + 0x100
OK, INVALID, PENDING, BUSY, DEADLOCK = 1, 0, -1, -2, -3


def signed(value):
    return (value & 0xffffffff) - ((value & 0x80000000) << 1)


class SchedulerMachine(Machine):
    def __init__(self, image):
        self.system = {'fpcr': 0, 'fpsr': 0, 'elr_el1': 0, 'spsr_el1': 0, 'sp_el0': 0}
        super().__init__(image)
        self.programs, self.events = {}, []
        self.uc.hook_add(UC_HOOK_CODE, self.halted,
                         begin=image.symbols['task_port_halt'], end=image.symbols['task_port_halt'])
        for address, callback in ((PROBE, self.probe), (RESUME, self.resume)):
            self.uc.hook_add(UC_HOOK_CODE, callback, begin=address, end=address)

    def privileged(self, uc, address, size, instruction):
        mnemonic, operands = instruction
        parts = operands.lower().split(', ')
        if mnemonic == 'mrs' and parts[1] in self.system:
            uc.reg_write(getattr(regs, 'UC_ARM64_REG_' + parts[0].upper()), self.system[parts[1]])
        elif mnemonic == 'msr' and parts[0] in self.system:
            self.system[parts[0]] = uc.reg_read(getattr(regs, 'UC_ARM64_REG_' + parts[1].upper()))
        else:
            return super().privileged(uc, address, size, instruction)
        uc.reg_write(regs.UC_ARM64_REG_PC, address + size)

    def start(self):
        assert self.call('task_init') == OK

    def create(self, steps, affinity=None):
        key = len(self.programs) + 1
        program = {'steps': list(steps), 'key': key, 'results': [], 'started': False}
        self.programs[key] = program
        program['id'] = (self.call('task_create', PROBE, key) if affinity is None else
                         self.call('task_create_affinity', PROBE, key, affinity))
        assert program['id']
        return program

    def running(self):
        current = struct.unpack('<Q', self.uc.mem_read(self.image.symbols['current'] + self.core * 8, 8))[0]
        # The Task layout starts with TaskContext, entry and argument.
        key = struct.unpack('<Q', self.uc.mem_read(current + 216, 8))[0]
        return self.programs[key]

    def probe(self, uc, address, size, _):
        program = self.programs[uc.reg_read(regs.UC_ARM64_REG_X0)]
        assert not program['started'], 'an exited/started task restarted'
        program['started'] = True
        program['return'] = uc.reg_read(regs.UC_ARM64_REG_X30)
        saved_registers = [getattr(regs, f'UC_ARM64_REG_X{i}') for i in range(19, 30)]
        saved_registers += [getattr(regs, f'UC_ARM64_REG_D{i}') for i in range(8, 16)]
        program['saved'] = [(register, uc.reg_read(register)) for register in saved_registers]
        for i in range(19, 30): uc.reg_write(getattr(regs, f'UC_ARM64_REG_X{i}'), (program['key'] << 32) + i)
        for i in range(8, 16): uc.reg_write(getattr(regs, f'UC_ARM64_REG_D{i}'), (program['key'] << 32) + i)
        self.system.update(fpcr=program['key'] << 22, fpsr=program['key'],
                           elr_el1=program['key'] * 0x1000, spsr_el1=program['key'],
                           sp_el0=program['key'] * 0x2000)
        self.step(program)

    def check_registers(self, program):
        for i in range(19, 30):
            assert self.uc.reg_read(getattr(regs, f'UC_ARM64_REG_X{i}')) == (program['key'] << 32) + i
        for i in range(8, 16):
            assert self.uc.reg_read(getattr(regs, f'UC_ARM64_REG_D{i}')) == (program['key'] << 32) + i

    def resume(self, uc, address, size, _):
        program = self.running()
        self.check_registers(program)
        assert self.system['fpcr'] == program['key'] << 22
        assert self.system['fpsr'] == program['key']
        assert self.system['elr_el1'] == program['key'] * 0x1000
        assert self.system['spsr_el1'] == program['key']
        assert self.system['sp_el0'] == program['key'] * 0x2000
        program['results'].append(signed(uc.reg_read(regs.UC_ARM64_REG_X0)))
        self.step(program)

    def step(self, program):
        name, arguments = program['steps'].pop(0) if program['steps'] else ('return', ())
        arguments = [argument() if callable(argument) else argument for argument in arguments]
        self.events.append((program['key'], name))
        if name == 'return':
            for register, value in program['saved']: self.uc.reg_write(register, value)
            self.uc.reg_write(regs.UC_ARM64_REG_PC, program['return'])
            return
        for i, value in enumerate(arguments):
            self.uc.reg_write(getattr(regs, f'UC_ARM64_REG_X{i}'), value & 0xffffffffffffffff)
        self.uc.reg_write(regs.UC_ARM64_REG_X30, RESUME)
        self.uc.reg_write(regs.UC_ARM64_REG_PC, self.image.symbols[name])


class TaskTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image = Image(sys.argv[1])

    def setUp(self):
        self.m = SchedulerMachine(self.image)
        self.m.start()

    def test_boot_self_test_and_reinit_rejected(self):
        self.assertEqual(self.m.call('task_self_test'), OK)
        self.assertEqual(self.m.call('task_init'), INVALID)
        self.assertEqual(self.m.call('task_run'), OK)

    def test_round_robin_and_actual_stack_register_switching(self):
        a = self.m.create([('task_yield', ())] * 4 + [('task_exit', (31,))])
        b = self.m.create([('task_yield', ())] * 4 + [('task_exit', (-9,))])
        self.assertEqual(self.m.call('task_run'), OK)
        self.assertEqual([key for key, _ in self.m.events], [1, 2] * 5)
        self.assertEqual(a['results'], [OK] * 4)
        self.assertEqual(b['results'], [OK] * 4)
        self.assertEqual(self.m.call('task_reap', a['id'], STATS), OK)
        self.assertEqual(struct.unpack('<i', self.m.uc.mem_read(STATS, 4))[0], 31)
        self.assertEqual(self.m.call('task_reap', b['id'], STATS), OK)
        self.assertEqual(struct.unpack('<i', self.m.uc.mem_read(STATS, 4))[0], -9)

    def test_capacity_reaping_stale_ids_and_task_return(self):
        tasks = [self.m.create([]) for _ in range(32)]
        self.assertEqual(self.m.call('task_create', PROBE, 99), 0)
        self.assertEqual(signed(self.m.call('task_reap', tasks[0]['id'], 0)), PENDING)
        self.assertEqual(self.m.call('task_run'), OK)
        # Exit does not make a slot reusable until somebody reaps it.
        self.assertEqual(self.m.call('task_create', PROBE, 99), 0)
        for task in tasks: self.assertEqual(self.m.call('task_reap', task['id'], 0), OK)
        fresh = self.m.create([])
        self.assertNotEqual(fresh['id'], tasks[0]['id'])
        self.assertEqual(self.m.call('task_reap', tasks[0]['id'], 0), INVALID)
        self.assertEqual(self.m.call('task_state', tasks[0]['id'], STATS), INVALID)
        self.assertEqual(self.m.call('task_run'), OK)
        self.assertEqual(self.m.call('task_reap', fresh['id'], STATS), OK)
        self.assertEqual(struct.unpack('<i', self.m.uc.mem_read(STATS, 4))[0], 0)

    def test_join_blocks_wakes_and_exclusively_reaps(self):
        target = None
        waiter = self.m.create([('task_join', (lambda: target['id'], STATS))])
        target = self.m.create([('task_yield', ()), ('task_exit', (73,))])
        thief = self.m.create([('task_reap', (lambda: target['id'], 0)),
                               ('task_join', (lambda: target['id'], 0))])
        self.assertEqual(self.m.call('task_run'), OK)
        self.assertEqual(waiter['results'], [OK])
        self.assertEqual(thief['results'], [BUSY, BUSY])
        self.assertEqual(struct.unpack('<i', self.m.uc.mem_read(STATS, 4))[0], 73)
        self.assertEqual(self.m.call('task_reap', target['id'], 0), INVALID)

    def test_wait_cycles_and_self_join_are_rejected(self):
        a, b = None, None
        a = self.m.create([('task_join', (lambda: a['id'], 0)),
                           ('task_join', (lambda: b['id'], 0))])
        b = self.m.create([('task_join', (lambda: a['id'], 0))])
        self.assertEqual(self.m.call('task_run'), OK)
        self.assertEqual(a['results'], [DEADLOCK, OK])
        self.assertEqual(b['results'], [DEADLOCK])

    def test_invalid_calls_and_nested_dispatch(self):
        self.assertEqual(self.m.call('task_create', 0, 0), 0)
        for id in (0, 15, 0xffffffffffffffff):
            self.assertEqual(self.m.call('task_reap', id, 0), INVALID)
        self.assertEqual(self.m.call('task_current'), 0)
        self.assertEqual(self.m.call('task_yield'), INVALID)
        self.assertEqual(self.m.call('task_join', 17, 0), INVALID)
        task = self.m.create([('task_run', ()), ('task_current', ())])
        self.assertEqual(self.m.call('task_run'), OK)
        self.assertEqual(task['results'], [INVALID, task['id']])

    def test_repeated_switches_and_join_already_exited(self):
        target = self.m.create([('task_yield', ())] * 256)
        waiter = self.m.create([('task_yield', ())] * 257 +
                                [('task_join', (lambda: target['id'], STATS))])
        self.assertEqual(self.m.call('task_run'), OK)
        self.assertEqual(target['results'], [OK] * 256)
        self.assertEqual(waiter['results'], [OK] * 258)
        self.assertEqual(struct.unpack('<i', self.m.uc.mem_read(STATS, 4))[0], 0)
        self.assertEqual(self.m.call('task_reap', target['id'], 0), INVALID)

    def test_generation_never_wraps_to_a_stale_id(self):
        # A retired slot must stay unavailable instead of aliasing an old ID.
        generation = self.image.symbols['tasks'] + 232
        self.m.uc.mem_write(generation, struct.pack('<Q', (1 << 58) - 1))
        fresh = self.m.create([])
        self.assertEqual(fresh['id'] & 63, 2)
        self.assertEqual(self.m.call('task_run'), OK)

    def test_core_el_and_interrupt_ownership_rejected(self):
        for core, level, daif in ((4, 4, 0x3c0), (0, 0, 0x3c0), (0, 4, 0)):
            self.m.core, self.m.level, self.m.daif = core, level, daif
            self.assertEqual(self.m.call('task_create', PROBE, 1), 0)
            self.assertEqual(self.m.call('task_run'), INVALID)
            self.assertEqual(self.m.call('task_current'), 0)

    def test_secondary_can_create_but_not_coordinate_or_access_devices(self):
        self.m.core = 1
        task = self.m.call('task_create', PROBE, 1)
        self.assertGreater(task, 0)
        self.assertEqual(self.m.call('task_run'), INVALID)
        self.assertEqual(self.m.call('task_services_allowed'), 0)
        self.assertEqual(signed(self.m.call('handle_sys_call', 32, 0)), -7)

    def test_unavailable_and_invalid_affinity_is_rejected(self):
        for mask in (0, 2, 15, 16, 0xffffffff):
            self.assertEqual(self.m.call('task_create_affinity', PROBE, 1, mask), 0)

    def test_unknown_and_unavailable_syscalls(self):
        for id in (0, 6, 15, 16, 17, 18, 19, 0xffffffffffffffff):
            self.assertEqual(self.m.call('handle_sys_call', id, 0), 0)

    def test_stack_guard_corruption_halts_before_dispatch(self):
        task = self.m.create([])
        # First context_init stack argument points just past the 64-byte guard.
        table = self.image.symbols['tasks']
        stack_top = struct.unpack('<Q', self.m.uc.mem_read(table + 96, 8))[0]
        self.m.uc.mem_write(stack_top - 16384, b'\0')
        with self.assertRaisesRegex(AssertionError, 'halted'):
            self.m.call('task_run')
        self.assertFalse(task['started'])

    def test_bad_saved_stack_pointer_halts_before_dispatch(self):
        task = self.m.create([])
        sp = self.image.symbols['tasks'] + 96
        self.m.uc.mem_write(sp, struct.pack('<Q', 0x1001))
        with self.assertRaisesRegex(AssertionError, 'halted'):
            self.m.call('task_run')
        self.assertFalse(task['started'])


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *sys.argv[2:]])
