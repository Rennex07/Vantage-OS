"""Check default/opt-in linked boot sequencing with scripted initialization.

Usage: python storage_boot.py default-kernel.elf validation-kernel.elf
The validation ELF uses SD_BASE_CLOCK_HZ=100000000 STORAGE_PARTITION=1.
This does not boot firmware or validate hardware.
"""
import sys
import unittest

from unicorn import UC_HOOK_CODE
from unicorn import arm64_const as regs
from memory_management import Image, Machine, RETURN


def boot(image, mount=0, probe=0, task_probe=1):
    machine = Machine(image)
    events, messages = [], []
    values = {'uart_init': 0, 'uart_writeText': 0, 'mm_prepare': 1,
              'secondary_core_start': 1, 'env_Init': 0, 'mmu_is_enabled': 1,
              'parallel_init': 1, 'parallel_self_test': 1, 'pi4_drivers_start': 0,
              'task_init': 1, 'task_self_test': 1, 'task_multicore_self_test': task_probe, 'storage_start': mount,
              'storage_reboot_probe': probe, 'task_create': 1}

    def stub(uc, address, size, name):
        args = (uc.reg_read(regs.UC_ARM64_REG_X0), uc.reg_read(regs.UC_ARM64_REG_X1))
        events.append((name, args))
        if name == 'uart_writeText':
            messages.append(bytes(uc.mem_read(args[0], 256)).split(b'\0', 1)[0])
        if name == 'task_create':
            uc.emu_stop()
            return
        uc.reg_write(regs.UC_ARM64_REG_X0, values[name] & 0xffffffffffffffff)
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    for name in values:
        address = image.symbols[name]
        machine.uc.hook_add(UC_HOOK_CODE, stub, user_data=name, begin=address, end=address)
    machine.uc.reg_write(regs.UC_ARM64_REG_SP, image.symbols['__kernel_stack_top'])
    machine.uc.reg_write(regs.UC_ARM64_REG_X0, 0x4000000)
    machine.uc.emu_start(image.symbols['main'], RETURN, timeout=30_000_000, count=100000)
    return machine, events, b'\n'.join(messages)


class BootTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.default, cls.validation = Image(sys.argv[1]), Image(sys.argv[2])

    def test_default_boot_never_enters_storage(self):
        m, events, log = boot(self.default)
        self.assertFalse(m.failure)
        self.assertIn('task_create', [name for name, _ in events])
        self.assertNotIn('storage_start', [name for name, _ in events])
        self.assertIn(b'SD storage disabled', log)

    def test_opt_in_mount_probe_and_recovery_warning(self):
        for recovered, existing in ((0, 0), (1, 1)):
            m, events, log = boot(self.validation, recovered, existing)
            self.assertFalse(m.failure)
            self.assertIn(('storage_start', (100000000, 1)), events)
            names = [name for name, _ in events]
            self.assertLess(names.index('storage_start'), names.index('storage_reboot_probe'))
            self.assertLess(names.index('storage_reboot_probe'), names.index('task_create'))
            self.assertEqual(b'[WARN] Storage recovered' in log, bool(recovered))

    def test_failed_mount_or_probe_halts_before_app(self):
        for mount, probe in ((-8, 0), (0, -9)):
            m, events, _ = boot(self.validation, mount, probe)
            self.assertTrue(m.failure)
            self.assertNotIn('task_create', [name for name, _ in events])
            if mount < 0:
                self.assertNotIn('storage_reboot_probe', [name for name, _ in events])

    def test_failed_multicore_probe_halts_before_storage_and_app(self):
        m, events, _ = boot(self.validation, task_probe=0)
        self.assertTrue(m.failure)
        names = [name for name, _ in events]
        self.assertNotIn('storage_start', names)
        self.assertNotIn('task_create', names)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *sys.argv[3:]])
