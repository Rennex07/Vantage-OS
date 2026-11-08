"""Interleave four CPU contexts over shared modeled RAM and linked ARM code.

The production scheduler, lock and register-switch assembly execute. CPU/device
state and execution quanta are modeled; this is not physical cache-coherence,
interrupt or performance validation.
"""
import struct
import sys
import time
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ
from unicorn import arm64_const as regs
from memory_management import Image, RETURN, STATS
from tasks import SchedulerMachine, OK, DEADLOCK, INVALID

WORKER_RETURN = 0x7000400


class MulticoreMachine(SchedulerMachine):
    def __init__(self, image):
        super().__init__(image)
        self.systems = [dict(self.system) for _ in range(4)]
        self.trace = []
        self.budget = None
        # The independent secondary-startup tests exercise firmware/MMU setup.
        # Here the four modeled workers have already joined that common setup.
        self.uc.mem_write(image.symbols['enabled'], struct.pack('<I', 1))
        self.lock_version = 0
        self.reservations = [None] * 4
        lock = image.symbols['scheduler_lock']
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.lock_write, begin=lock, end=lock + 3)
        decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        decoder.skipdata = True
        section = image.named['.text']
        for ins in decoder.disasm(image.section_bytes(section), section[3]):
            if ins.mnemonic in ('ldaxr', 'stxr'):
                self.uc.hook_add(UC_HOOK_CODE, self.exclusive,
                                 user_data=(ins.mnemonic, ins.op_str), begin=ins.address, end=ins.address)
        self.uc.hook_add(UC_HOOK_CODE, self.worker_return,
                         begin=WORKER_RETURN, end=WORKER_RETURN)
        self.uc.hook_add(UC_HOOK_CODE, self.quantum)
        self.timer = 0
        self.uc.mem_map(0xfe003000, 4096)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.read_timer, begin=0xfe003004, end=0xfe003007)

    def read_timer(self, uc, access, address, size, value, _):
        self.timer += 1000
        uc.mem_write(address, struct.pack('<I', self.timer & 0xffffffff))

    def quantum(self, uc, address, size, _):
        if self.budget is not None:
            if self.budget == 0:
                uc.emu_stop()
            else:
                self.budget -= 1

    def step(self, program):
        self.trace.append((self.core, program['key']))
        super().step(program)

    def lock_write(self, uc, access, address, size, value, _):
        self.lock_version += 1

    def exclusive(self, uc, address, size, instruction):
        # Unicorn CPU contexts do not supply independent exclusive monitors.
        # Model per-CPU reservations explicitly while executing the ARM lock's
        # load/store/retry sequence and its surrounding production C code.
        mnemonic, operands = instruction
        parts = operands.split(', ')
        register = lambda name: getattr(regs, 'UC_ARM64_REG_' + name.upper())
        base = parts[-1].strip('[]')
        pointer = uc.reg_read(register(base))
        assert pointer == self.image.symbols['scheduler_lock']
        if mnemonic == 'ldaxr':
            uc.reg_write(register(parts[0]), struct.unpack('<I', uc.mem_read(pointer, 4))[0])
            self.reservations[self.core] = self.lock_version
        else:
            success = self.reservations[self.core] == self.lock_version
            if success:
                uc.mem_write(pointer, struct.pack('<I', uc.reg_read(register(parts[1]))))
                self.lock_version += 1
            self.reservations[self.core] = None
            uc.reg_write(register(parts[0]), 0 if success else 1)
        uc.reg_write(regs.UC_ARM64_REG_PC, address + size)

    def worker_return(self, uc, address, size, _):
        assert self.core > 0
        uc.reg_write(regs.UC_ARM64_REG_X0, self.core)
        uc.reg_write(regs.UC_ARM64_REG_X30, WORKER_RETURN)
        uc.reg_write(regs.UC_ARM64_REG_PC, self.image.symbols['parallel_worker_poll'])

    def run_smp(self, entry='task_run', quantum=521, args=(), max_rounds=10000):
        contexts = []
        saved = self.uc.context_save()
        self.systems[0] = dict(self.system)
        for core in range(4):
            self.uc.context_restore(saved)
            stack = self.image.symbols['__kernel_stack_top' if core == 0 else f'__core{core}_stack_top']
            self.uc.reg_write(regs.UC_ARM64_REG_SP, stack)
            self.uc.reg_write(regs.UC_ARM64_REG_X30, RETURN if core == 0 else WORKER_RETURN)
            self.uc.reg_write(regs.UC_ARM64_REG_X0, core)
            if core == 0:
                for i, value in enumerate(args):
                    self.uc.reg_write(getattr(regs, f'UC_ARM64_REG_X{i}'), value)
            self.uc.reg_write(regs.UC_ARM64_REG_PC,
                              self.image.symbols[entry if core == 0 else 'parallel_worker_poll'])
            contexts.append(self.uc.context_save())
        states = [(4, 0x3c0, None)] * 4
        self.positions = [0] * 4
        start = time.monotonic()
        for round_number in range(max_rounds):
            for core in range(4):
                self.uc.context_restore(contexts[core])
                self.core, self.system = core, self.systems[core]
                self.level, self.daif, self.kernel_sp = states[core]
                self.budget = quantum + (round_number * 13 + core * 7) % 31
                self.uc.emu_start(self.uc.reg_read(regs.UC_ARM64_REG_PC), RETURN + 4,
                                  timeout=3000000)
                self.budget = None
                if self.failure:
                    raise AssertionError(self.failure)
                contexts[core] = self.uc.context_save()
                self.positions[core] = self.uc.reg_read(regs.UC_ARM64_REG_PC)
                states[core] = (self.level, self.daif, self.kernel_sp)
                if core == 0 and self.uc.reg_read(regs.UC_ARM64_REG_PC) == RETURN:
                    # task_run must not return while a secondary still owns a task.
                    owners = struct.unpack('<4Q', self.uc.mem_read(self.image.symbols['current'], 32))
                    assert owners == (0, 0, 0, 0), owners
                    return self.uc.reg_read(regs.UC_ARM64_REG_X0)
                if not struct.unpack('<I', self.uc.mem_read(self.image.symbols['scheduler_lock'], 4))[0]:
                    owners = [owner for owner in struct.unpack('<4Q', self.uc.mem_read(
                        self.image.symbols['current'], 32)) if owner]
                    assert len(owners) == len(set(owners)), 'task owned by two cores'
            if time.monotonic() - start > 120:
                break
        raise AssertionError('four-core scheduler did not reach global quiescence: ' +
                             repr([hex(position) for position in self.positions]))


class MulticoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image = Image(sys.argv[1])

    def setUp(self):
        self.m = MulticoreMachine(self.image)
        self.m.start()

    def test_all_four_cores_keep_independent_stacks_registers_and_affinity(self):
        programs = [self.m.create([('task_yield', ())] * 16 + [('task_exit', (core + 40,))],
                                  affinity=1 << core) for core in range(4)]
        self.assertEqual(self.m.run_smp(), OK)
        for core, program in enumerate(programs):
            self.assertEqual(program['results'], [OK] * 16)
            self.assertEqual({cpu for cpu, key in self.m.trace if key == program['key']}, {core})
            self.assertEqual(self.m.call('task_reap', program['id'], STATS), OK)
            self.assertEqual(struct.unpack('<i', self.m.uc.mem_read(STATS, 4))[0], core + 40)

    def test_movable_tasks_preserve_actual_context_when_resuming_on_other_cores(self):
        programs = [self.m.create([('task_yield', ())] * 24, affinity=15) for _ in range(8)]
        self.assertEqual(self.m.run_smp(quantum=509), OK)
        self.assertEqual({cpu for cpu, _ in self.m.trace}, {0, 1, 2, 3})
        migrations = 0
        for program in programs:
            self.assertEqual(program['results'], [OK] * 24)
            migrations += len({cpu for cpu, key in self.m.trace if key == program['key']}) > 1
        self.assertGreater(migrations, 0)

    def test_cross_core_join_wakes_after_target_context_is_saved(self):
        target = None
        waiter = self.m.create([('task_join', (lambda: target['id'], STATS))], affinity=1)
        target = self.m.create([('task_yield', ())] * 32 + [('task_exit', (73,))], affinity=8)
        self.assertEqual(self.m.run_smp(), OK)
        self.assertEqual(waiter['results'], [OK])
        self.assertEqual(struct.unpack('<i', self.m.uc.mem_read(STATS, 4))[0], 73)
        self.assertEqual(self.m.call('task_reap', target['id'], 0), INVALID)

    def test_cross_core_cycle_is_rejected_without_stalling_dispatchers(self):
        a = b = None
        a = self.m.create([('task_join', (lambda: b['id'], 0))], affinity=2)
        b = self.m.create([('task_join', (lambda: a['id'], 0))], affinity=4)
        self.assertEqual(self.m.run_smp(), OK)
        self.assertEqual(sorted(a['results'] + b['results']), [DEADLOCK, OK])

    def test_boot_probe_requires_simultaneous_entry_and_all_core_progress(self):
        self.assertEqual(self.m.run_smp(entry='task_multicore_self_test'), OK)
        self.assertEqual(struct.unpack('<4I', self.m.uc.mem_read(self.image.symbols['core_passed'], 16)),
                         (1, 1, 1, 1))

    def test_remote_syscalls_and_raw_file_calls_leave_storage_untouched(self):
        before = bytes(self.m.uc.mem_read(self.image.symbols['volume'], 72))
        program = self.m.create([('handle_sys_call', (32, 0)), ('handle_sys_call', (2, 0xff)),
                                 ('files_open', (99, 0, 7)), ('files_unmount', ())], affinity=2)
        self.assertEqual(self.m.run_smp(), OK)
        self.assertEqual(program['results'], [-7, 0, -7, -7])
        self.assertEqual(bytes(self.m.uc.mem_read(self.image.symbols['volume'], 72)), before)

    def test_movable_tasks_cannot_initialize_raw_sd_or_mount_storage(self):
        program = self.m.create([('task_services_allowed', ()), ('sd_init', (100000000,)),
                                 ('storage_start', (100000000, 0))], affinity=15)
        # No EMMC2 registers are mapped: accidentally touching MMIO fails too.
        self.assertEqual(self.m.run_smp(), OK)
        self.assertEqual(program['results'], [0, -3, -7])

    def test_el0_yield_and_exit_can_migrate_with_pending_exception_frame(self):
        user = self.m.create([('sys_task_id', ()), ('sys_task_yield', ())] * 16 +
                             [('sys_task_exit', (-17,))], user=True, affinity=15)
        for _ in range(4): self.m.create([('task_yield', ())] * 24, affinity=15)
        self.assertEqual(self.m.run_smp(quantum=503), OK)
        self.assertEqual(user['results'], [user['id'], OK] * 16)
        self.assertEqual(self.m.call('task_reap', user['id'], STATS), OK)
        self.assertEqual(struct.unpack('<i', self.m.uc.mem_read(STATS, 4))[0], -17)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *sys.argv[2:]])
