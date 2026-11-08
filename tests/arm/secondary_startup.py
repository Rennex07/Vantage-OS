"""Exercise the linked AArch64 startup code with mocked firmware and MMIO.

Usage: python tests/arm/secondary_startup.py path/to/fresh/kernel8.elf
Requires unicorn==2.1.4 and capstone==5.0.7. This is not a Pi runtime test.
"""

import pathlib
import struct
import sys
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as regs


TIMER_LOW = 0xFE003004
RETURN = 0x1000000
TIMEOUT_US = 1000000


class Image:
    def __init__(self, path):
        self.data = pathlib.Path(path).read_bytes()
        if self.data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", self.data, 18)[0] != 183:
            raise ValueError("Expected a little-endian AArch64 ELF64 image")
        offset = struct.unpack_from("<Q", self.data, 40)[0]
        size, count = struct.unpack_from("<HH", self.data, 58)
        self.sections = [struct.unpack_from("<IIQQQQIIQQ", self.data, offset + i * size)
                         for i in range(count)]
        self.symbols = {}
        self.sizes = {}
        for section in self.sections:
            if section[1] != 2:
                continue
            strings = self.sections[section[6]]
            names = self.data[strings[4]:strings[4] + strings[5]]
            for p in range(section[4], section[4] + section[5], section[9]):
                name, _, _, _, value, length = struct.unpack_from("<IBBHQQ", self.data, p)
                name = names[name:names.find(b"\0", name)].decode()
                self.symbols[name] = value
                self.sizes[name] = length

    def machine(self):
        if self.symbols["__bss_end"] >= RETURN:
            raise ValueError("Test return address overlaps the kernel; update the harness layout")
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(0, 0x1100000)
        u.mem_map(0xFE003000, 0x1000)
        for s in self.sections:
            if s[2] & 2 and s[1] != 8 and s[5]:
                u.mem_write(s[3], self.data[s[4]:s[4] + s[5]])
        return u


class Execution:
    def __init__(self, image, core=0, el=4, control=0):
        self.image = image
        self.u = image.machine()
        self.decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        self.instructions = {}
        self.system = {"mpidr_el1": core, "currentel": el,
                       "sctlr_el1": control, "sctlr_el2": control}
        self.parked = False
        self.events = 0
        self.callback = lambda ins: False
        self.u.hook_add(UC_HOOK_CODE, self.hook)

    def hook(self, u, addr, size, _):
        if addr not in self.instructions:
            self.instructions[addr] = next(self.decoder.disasm(bytes(u.mem_read(addr, size)), addr))
        ins = self.instructions[addr]
        if self.callback(ins):
            return
        if ins.mnemonic == "mrs":
            dest, register = ins.op_str.split(", ")
            u.reg_write(getattr(regs, "UC_ARM64_REG_" + dest.upper()), self.system[register])
            u.reg_write(regs.UC_ARM64_REG_PC, addr + 4)
        elif ins.mnemonic == "msr":
            register, source = ins.op_str.split(", ")
            if register not in ("daifset", "spsel", "cptr_el2", "hcr_el2", "sctlr_el1",
                                 "spsr_el2", "elr_el2", "cpacr_el1", "vbar_el1",
                                 "mair_el1", "tcr_el1", "ttbr0_el1"):
                raise AssertionError("Unexpected privileged write: " + register)
            value = int(source[1:], 0) if source.startswith("#") else u.reg_read(
                getattr(regs, "UC_ARM64_REG_" + source.upper()))
            self.system[register] = value
            u.reg_write(regs.UC_ARM64_REG_PC, addr + 4)
        elif ins.mnemonic == "eret":
            if self.system["spsr_el2"] != 0x3C5:
                raise AssertionError("Secondary must enter EL1h with interrupts masked")
            self.system["currentel"] = 4
            u.reg_write(regs.UC_ARM64_REG_PC, self.system["elr_el2"])
        elif ins.mnemonic in ("ic", "tlbi"):
            # Register/ordering checks only; no actual cache or TLB model.
            u.reg_write(regs.UC_ARM64_REG_PC, addr + 4)
        elif ins.mnemonic == "sev":
            self.events += 1
            u.reg_write(regs.UC_ARM64_REG_PC, addr + 4)
        elif ins.mnemonic == "wfe":
            self.parked = True
            u.emu_stop()

    def run(self, symbol):
        self.u.emu_start(self.image.symbols[symbol], RETURN, count=10000)

    def flag(self, core):
        addr = self.image.symbols["secondary_ready"] + core * 64
        return struct.unpack("<I", self.u.mem_read(addr, 4))[0]


class StartupTests(unittest.TestCase):
    def worker(self, core, el=8, control=0):
        execution = Execution(IMAGE, core=core, el=el, control=control)
        if 1 <= core <= 3:
            top = IMAGE.symbols[f"__core{core}_stack_top"]
            execution.u.mem_write(top - 16, b"\xA5" * 16)
        execution.run("secondary_entry")
        self.assertTrue(execution.parked)
        return execution

    def primary(self, target, response=True, timer_start=0, elapsed=TIMEOUT_US,
                fallback=False, response_delay=1, **system):
        core = target
        execution = Execution(IMAGE, **system)
        u = execution.u
        u.reg_write(regs.UC_ARM64_REG_X0, core)
        u.reg_write(regs.UC_ARM64_REG_X30, RETURN)
        u.mem_write(TIMER_LOW, struct.pack("<I", timer_start))
        # Stale success from the target, or flags from other cores, must not
        # satisfy this core's handshake after its own status is cleared.
        for slot in range(4):
            u.mem_write(IMAGE.symbols["secondary_ready"] + slot * 64, struct.pack("<I", 1))
        seen_clear = []
        polls = []

        def callback(ins):
            if ins.mnemonic == "sev":
                seen_clear.append(execution.flag(core) == 0)
            elif ins.mnemonic == "ldr" and ins.op_str == "w0, [x1]":
                polls.append(True)
                if response and len(polls) >= response_delay:
                    worker = self.worker(core)
                    addr = IMAGE.symbols["secondary_ready"] + core * 64
                    u.mem_write(addr, bytes(worker.u.mem_read(addr, 4)))
                elif fallback:
                    # Exercise exhaustion without emulating 67 million iterations.
                    u.reg_write(regs.UC_ARM64_REG_X2, 1)
            elif ins.mnemonic == "ldr" and ins.op_str == "w6, [x3]":
                u.mem_write(TIMER_LOW, struct.pack("<I", (timer_start + elapsed) & 0xFFFFFFFF))
            return False

        execution.callback = callback
        execution.run("secondary_core_start")
        self.assertEqual(u.reg_read(regs.UC_ARM64_REG_PC), RETURN, "startup did not return")
        self.assertTrue(all(seen_clear))
        return execution

    def test_layout(self):
        previous = IMAGE.symbols["__kernel_stack_top"]
        for core in (1, 2, 3):
            top = IMAGE.symbols[f"__core{core}_stack_top"]
            self.assertEqual(top - previous, 4096)
            self.assertEqual(top % 16, 0)
            self.assertLessEqual(top, IMAGE.symbols["__user_start"])
            previous = top
        flags = IMAGE.symbols["secondary_ready"]
        self.assertEqual(flags % 64, 0)
        self.assertGreaterEqual(flags, IMAGE.symbols["__bss_start"])
        self.assertLessEqual(flags + 256, IMAGE.symbols["__kernel_stack_top"] - 0x4000)
        gate = IMAGE.symbols["secondary_mmu_ready"]
        self.assertEqual(IMAGE.symbols["__smp_boot_start"] % 0x200000, 0)
        self.assertLessEqual(IMAGE.symbols["__user_end"], IMAGE.symbols["__smp_boot_start"])
        self.assertGreaterEqual(gate, IMAGE.symbols["__smp_boot_start"])
        self.assertLess(gate, IMAGE.symbols["__smp_boot_end"])
        self.assertEqual(IMAGE.sizes["workers"], 4 * 64)

    def test_workers_have_distinct_stacks_and_flags(self):
        for core in (1, 2, 3):
            for el in (4, 8):
                with self.subTest(core=core, el=el):
                    execution = self.worker(core, el)
                    top = IMAGE.symbols[f"__core{core}_stack_top"]
                    self.assertEqual(execution.u.reg_read(regs.UC_ARM64_REG_SP), top)
                    self.assertEqual(bytes(execution.u.mem_read(top - 16, 16)), bytes(16))
                    self.assertEqual([execution.flag(c) for c in range(4)],
                                     [int(c == core) for c in range(4)])
                    self.assertEqual(execution.events, 1)
                    self.assertEqual(execution.system["currentel"], 4)
                    self.assertEqual(execution.system["vbar_el1"], IMAGE.symbols["vector_table"])
                    self.assertEqual(execution.system["cpacr_el1"], 3 << 20)

    def test_workers_wait_for_gate_then_enable_mmu_before_c(self):
        for core in (1, 2, 3):
            execution = self.worker(core)
            u = execution.u
            called = []

            def callback(ins):
                if ins.address == IMAGE.symbols["mmu_enable_shared"]:
                    called.append("mmu")
                    u.reg_write(regs.UC_ARM64_REG_PC, u.reg_read(regs.UC_ARM64_REG_X30))
                    return True
                if ins.address == IMAGE.symbols["parallel_worker_entry"]:
                    called.append("worker")
                    self.assertEqual(u.reg_read(regs.UC_ARM64_REG_X0), core)
                    u.emu_stop()
                    return True
                return False

            execution.callback = callback
            u.mem_write(IMAGE.symbols["secondary_mmu_ready"], struct.pack("<I", 1))
            u.emu_start(u.reg_read(regs.UC_ARM64_REG_PC) + 4, RETURN, count=1000)
            self.assertEqual(called, ["mmu", "worker"])

    def test_shared_mmu_registers_and_gate_publication(self):
        execution = Execution(IMAGE)
        u = execution.u
        u.reg_write(regs.UC_ARM64_REG_SP, IMAGE.symbols["__kernel_stack_top"])
        u.reg_write(regs.UC_ARM64_REG_X30, RETURN)
        u.reg_write(regs.UC_ARM64_REG_X0, 0x3E000000)
        u.reg_write(regs.UC_ARM64_REG_X1, 0x400000)
        gate = IMAGE.symbols["secondary_mmu_ready"]
        gate_writes = []

        def gate_store(u, access, address, size, value, _):
            if address == gate:
                gate_writes.append(value)
                self.assertEqual(execution.system["sctlr_el1"] & 0x1005, 0x1005)

        from unicorn import UC_HOOK_MEM_WRITE
        u.hook_add(UC_HOOK_MEM_WRITE, gate_store)
        # Isolate register/gate sequencing from the firmware-discovery state
        # machine. memory_management.py tests the actual mapping policy.
        def mapping_policy(ins):
            if ins.address == IMAGE.symbols["mm_block_mapped"]:
                u.reg_write(regs.UC_ARM64_REG_X0, 1)
                u.reg_write(regs.UC_ARM64_REG_PC, u.reg_read(regs.UC_ARM64_REG_X30))
                return True
            return False

        execution.callback = mapping_policy
        u.emu_start(IMAGE.symbols["mmu_init"], RETURN, count=200000)
        self.assertEqual(u.reg_read(regs.UC_ARM64_REG_PC), RETURN)
        self.assertEqual(gate_writes, [1])
        expected = {name: execution.system[name] for name in
                    ("mair_el1", "tcr_el1", "ttbr0_el1")}
        # Every secondary executes the same configuration function and root.
        for core in (1, 2, 3):
            secondary = Execution(IMAGE, core=core)
            secondary.u.reg_write(regs.UC_ARM64_REG_X30, RETURN)
            secondary.run("mmu_enable_shared")
            self.assertEqual({name: secondary.system[name] for name in expected}, expected)

    def test_acquire_release_and_completion_instructions(self):
        u = IMAGE.machine()
        decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        instructions = {}
        for name in ("parallel_load", "parallel_store", "parallel_wake", "parallel_completion_barrier"):
            instructions[name] = [(i.mnemonic, i.op_str) for i in decoder.disasm(
                bytes(u.mem_read(IMAGE.symbols[name], IMAGE.sizes[name])), IMAGE.symbols[name])]
        self.assertIn("ldar", [name for name, _ in instructions["parallel_load"]])
        self.assertIn("stlr", [name for name, _ in instructions["parallel_store"]])
        self.assertIn(("dsb", "sy"), instructions["parallel_completion_barrier"])
        wake = instructions["parallel_wake"]
        self.assertLess(wake.index(("dsb", "ishst")), wake.index(("sev", "")))

    def test_mmu_init_rejects_live_tables(self):
        for control in (1, 4, 5):
            execution = Execution(IMAGE, control=control)
            u = execution.u
            u.reg_write(regs.UC_ARM64_REG_SP, IMAGE.symbols["__kernel_stack_top"])
            u.reg_write(regs.UC_ARM64_REG_X30, RETURN)
            halted = []

            def reject(ins):
                if ins.address == IMAGE.symbols["mm_halt"]:
                    halted.append(True)
                    u.emu_stop()
                    return True
                return False

            execution.callback = reject
            execution.run("mmu_init")
            self.assertTrue(halted)
            self.assertNotIn("ttbr0_el1", execution.system)
            self.assertEqual(struct.unpack("<I", u.mem_read(
                IMAGE.symbols["secondary_mmu_ready"], 4))[0], 0)

    def test_workers_reject_invalid_state(self):
        for core, el, control in ((0, 8, 0), (4, 8, 0), (5, 8, 0),
                                  (1, 12, 0), (2, 0, 0), (1, 4, 1),
                                  (2, 8, 4), (3, 4, 4), (3, 8, 1)):
            with self.subTest(core=core, el=el, control=control):
                execution = self.worker(core, el, control)
                self.assertEqual([execution.flag(c) for c in range(4)], [0] * 4)
                self.assertEqual(execution.events, 0)

    def test_release_slots(self):
        for core in (1, 2, 3):
            with self.subTest(core=core):
                execution = self.primary(core)
                self.assertEqual(execution.u.reg_read(regs.UC_ARM64_REG_X0), 1)
                self.assertEqual(execution.events, 1)
                for slot in range(4):
                    value = struct.unpack("<Q", execution.u.mem_read(0xD8 + slot * 8, 8))[0]
                    self.assertEqual(value, IMAGE.symbols["secondary_entry"] if slot == core else 0)

    def test_primary_rejects_before_release(self):
        cases = [(core, {}) for core in (0, 4, 0xFFFFFFFF)]
        cases += [(1, state) for state in ({"el": 8}, {"core": 1},
                                          {"control": 1}, {"control": 4})]
        for target, system in cases:
            with self.subTest(target=target, system=system):
                execution = self.primary(target, **system)
                self.assertEqual(execution.u.reg_read(regs.UC_ARM64_REG_X0), 0)
                self.assertEqual(execution.events, 0)
                self.assertEqual(bytes(execution.u.mem_read(0xD8, 32)), bytes(32))

    def test_timeouts_and_timer_wrap(self):
        for core in (1, 2, 3):
            for start, fallback in ((0, False), (0xFFF80000, False), (0, True)):
                with self.subTest(core=core, start=start, fallback=fallback):
                    execution = self.primary(core, response=False, timer_start=start,
                                             elapsed=0 if fallback else TIMEOUT_US,
                                             fallback=fallback)
                    self.assertEqual(execution.u.reg_read(regs.UC_ARM64_REG_X0), 0)
                    self.assertEqual(execution.events, 1)
                    # A timeout cannot retract an entry already read by firmware.
                    slot = struct.unpack("<Q", execution.u.mem_read(0xD8 + core * 8, 8))[0]
                    self.assertEqual(slot, IMAGE.symbols["secondary_entry"])

    def test_response_just_before_deadline(self):
        for start in (0, 0xFFF80000):
            execution = self.primary(3, timer_start=start, elapsed=TIMEOUT_US - 1,
                                     response_delay=2)
            self.assertEqual(execution.u.reg_read(regs.UC_ARM64_REG_X0), 1)

    def test_main_sequences_and_halts_on_missing_worker(self):
        for missing in (None, 1, 2, 3):
            with self.subTest(missing=missing):
                execution = Execution(IMAGE)
                u = execution.u
                u.reg_write(regs.UC_ARM64_REG_SP, IMAGE.symbols["__kernel_stack_top"])
                released, messages, display = [], [], []

                def callback(ins):
                    addr = ins.address
                    # This test isolates secondary release sequencing. The MM
                    # harness separately executes real DTB discovery and boot.
                    if addr == IMAGE.symbols["mm_prepare"]:
                        u.reg_write(regs.UC_ARM64_REG_X0, 1)
                        u.reg_write(regs.UC_ARM64_REG_PC, u.reg_read(regs.UC_ARM64_REG_X30))
                        return True
                    if addr == IMAGE.symbols["uart_init"]:
                        u.reg_write(regs.UC_ARM64_REG_PC, u.reg_read(regs.UC_ARM64_REG_X30))
                        return True
                    if addr == IMAGE.symbols["uart_writeText"]:
                        p = u.reg_read(regs.UC_ARM64_REG_X0)
                        message = bytes(u.mem_read(p, 128)).split(b"\0", 1)[0].decode()
                        messages.append(message)
                        u.reg_write(regs.UC_ARM64_REG_PC, u.reg_read(regs.UC_ARM64_REG_X30))
                        return True
                    if addr == IMAGE.symbols["env_Init"]:
                        display.append(True)
                        u.emu_stop()
                        return True
                    if ins.mnemonic == "sev":
                        released.append(u.reg_read(regs.UC_ARM64_REG_X7))
                    elif ins.mnemonic == "ldr" and ins.op_str == "w0, [x1]":
                        core = u.reg_read(regs.UC_ARM64_REG_X7)
                        if core != missing:
                            worker = self.worker(core)
                            p = IMAGE.symbols["secondary_ready"] + core * 64
                            u.mem_write(p, bytes(worker.u.mem_read(p, 4)))
                        else:
                            u.reg_write(regs.UC_ARM64_REG_X2, 1)
                    return False

                execution.callback = callback
                execution.run("main")
                self.assertEqual(released, list(range(1, (missing or 3) + 1)))
                self.assertEqual(bool(display), missing is None)
                self.assertEqual(execution.parked, missing is not None)
                if missing is not None:
                    self.assertIn(f"Core {missing} startup unconfirmed", messages[-1])
                for core in range(1, missing or 4):
                    self.assertIn(f"[OK] Core {core} entry reached\n", messages)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    IMAGE = Image(sys.argv.pop(1))
    print("Mocked assembled-code tests: not firmware, cache coherence or Pi hardware.")
    unittest.main()
