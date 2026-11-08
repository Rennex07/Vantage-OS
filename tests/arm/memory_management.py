"""Execute the linked C memory subsystem with mocked privileged hardware.

Usage: python tests/arm/memory_management.py path/to/kernel8.elf -v
Requires unicorn==2.1.4 and capstone==5.0.7. This is not a Pi/cache-coherence test.
"""
import pathlib
import struct
import sys
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as regs

DTB = 0x4000000
FRAMEBUFFER = 0x6000000
RETURN = 0x7fff000
STATS = 0x7ffe000


class Image:
    def __init__(self, path):
        self.data = pathlib.Path(path).read_bytes()
        if self.data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', self.data, 18)[0] != 183:
            raise ValueError('Expected little-endian AArch64 ELF64')
        offset = struct.unpack_from('<Q', self.data, 40)[0]
        size, count, names_index = struct.unpack_from('<HHH', self.data, 58)
        self.sections = [struct.unpack_from('<IIQQQQIIQQ', self.data, offset + i * size) for i in range(count)]
        names = self.section_bytes(self.sections[names_index])
        self.named = {self.string(names, s[0]): s for s in self.sections}
        self.symbols = {}
        for section in self.sections:
            if section[1] == 2:
                strings = self.section_bytes(self.sections[section[6]])
                for pos in range(section[4], section[4] + section[5], section[9]):
                    name, _, _, _, value, _ = struct.unpack_from('<IBBHQQ', self.data, pos)
                    self.symbols[self.string(strings, name)] = value

    @staticmethod
    def string(data, offset):
        return data[offset:data.index(0, offset)].decode()

    def section_bytes(self, section):
        return self.data[section[4]:section[4] + section[5]]


def fixture(no_map=0x2000000, bad_magic=False, dynamic=False,
            initrd=None, initrd_cells=1, reservation=(0x1800123, 0x1000),
            ram_size=0x8000000):
    tree, strings = bytearray(), bytearray()

    def word(n): tree.extend(struct.pack('>I', n))
    def begin(name):
        word(1)
        tree.extend(name.encode() + b'\0')
        while len(tree) % 4: tree.append(0)
    def prop(name, value):
        word(3)
        word(len(value))
        word(len(strings))
        strings.extend(name.encode() + b'\0')
        tree.extend(value)
        while len(tree) % 4: tree.append(0)
    def cell(name, value): prop(name, struct.pack('>I', value))
    begin('')
    cell('#address-cells', 1)
    cell('#size-cells', 1)
    begin('memory@0')
    prop('device_type', b'memory\0')
    prop('reg', struct.pack('>II', 0, ram_size))
    word(2)
    if initrd is not None:
        begin('chosen')
        for name, value in zip(('linux,initrd-start', 'linux,initrd-end'), initrd):
            if value is not None:
                prop(name, struct.pack('>I' if initrd_cells == 1 else '>Q', value))
        word(2)
    begin('reserved-memory')
    cell('#address-cells', 1)
    cell('#size-cells', 1)
    prop('ranges', b'')
    begin('firmware')
    prop('reg', struct.pack('>II', no_map, 0x1000))
    prop('no-map', b'')
    word(2)
    if dynamic:
        begin('pool')
        cell('size', 0x100000)
        cell('alignment', 0x200000)
        prop('alloc-ranges', struct.pack('>II', 0x3000000, 0x1000000))
        prop('reusable', b'')
        word(2)
    word(2)
    word(2)
    word(9)
    structure = 72
    strings_start = structure + len(tree)
    header = struct.pack('>10I', 0 if bad_magic else 0xd00dfeed,
                         strings_start + len(strings), structure, strings_start, 40,
                         17, 16, 0, len(strings), len(tree))
    reservations = struct.pack('>QQQQ', *reservation, 0, 0)
    return header + reservations + tree + strings


class Machine:
    def __init__(self, image):
        self.image = image
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.uc.mem_map(0, 0x8000000)
        for section in image.sections:
            if section[2] & 2 and section[1] != 8 and section[5]:
                self.uc.mem_write(section[3], image.section_bytes(section))
        self.uc.reg_write(regs.UC_ARM64_REG_CPACR_EL1, 3 << 20)
        self.core, self.level, self.daif, self.sctlr = 0, 4, 0x3c0, 0
        self.elr_el2 = None
        self.failure = None
        self.stop_at_main = False
        self.minimum_sp = image.symbols['__kernel_stack_top']
        self.main_argument = None
        self.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, _: uc.emu_stop(), begin=RETURN, end=RETURN)
        self.uc.hook_add(UC_HOOK_CODE, self.halted, begin=image.symbols['mm_halt'], end=image.symbols['mm_halt'])
        self.uc.hook_add(UC_HOOK_CODE, self.at_main, begin=image.symbols['main'], end=image.symbols['main'])
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.stack_write,
                         begin=image.symbols['__kernel_stack_bottom'], end=image.symbols['__kernel_stack_top'] - 1)
        text = image.named['.text']
        decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        decoder.skipdata = True
        for instruction in decoder.disasm(image.section_bytes(text), text[3]):
            if instruction.mnemonic in ['mrs', 'msr', 'dsb', 'dmb', 'isb', 'tlbi', 'ic', 'dc', 'eret']:
                self.uc.hook_add(UC_HOOK_CODE, self.privileged,
                                 user_data=(instruction.mnemonic, instruction.op_str),
                                 begin=instruction.address, end=instruction.address)

    def stack_write(self, uc, access, address, size, value, _):
        self.minimum_sp = min(self.minimum_sp, address)

    def halted(self, uc, address, size, _):
        self.failure = f'Kernel halted at {address:#x}'
        uc.emu_stop()

    def at_main(self, uc, address, size, _):
        if self.stop_at_main:
            self.main_argument = uc.reg_read(regs.UC_ARM64_REG_X0)
            uc.emu_stop()

    def privileged(self, uc, address, size, instruction):
        mnemonic, operands = instruction
        parts = [p.strip().lower() for p in operands.split(',')]
        if mnemonic == 'mrs':
            dest, source = parts
            values = {'mpidr_el1': 0x80000000 | self.core, 'currentel': self.level,
                      'daif': self.daif, 'sctlr_el1': self.sctlr}
            if source not in values:
                self.failure = f'Unexpected MRS {operands}'
                uc.emu_stop()
                return
            uc.reg_write(getattr(regs, 'UC_ARM64_REG_' + dest.upper()), values[source])
        elif mnemonic == 'msr' and parts[0] == 'sctlr_el1':
            self.sctlr = uc.reg_read(getattr(regs, 'UC_ARM64_REG_' + parts[1].upper()))
        elif mnemonic == 'msr' and parts[0] == 'elr_el2':
            self.elr_el2 = uc.reg_read(getattr(regs, 'UC_ARM64_REG_' + parts[1].upper()))
        elif mnemonic == 'eret':
            if self.elr_el2 is None:
                self.failure = 'Unexpected ERET'
                uc.emu_stop()
            else:
                self.level = 4
                uc.reg_write(regs.UC_ARM64_REG_PC, self.elr_el2)
            return
        uc.reg_write(regs.UC_ARM64_REG_PC, address + size)

    def call(self, name, *args):
        self.uc.reg_write(regs.UC_ARM64_REG_SP, self.image.symbols['__kernel_stack_top'])
        self.uc.reg_write(regs.UC_ARM64_REG_X30, RETURN)
        for i, value in enumerate(args): self.uc.reg_write(getattr(regs, f'UC_ARM64_REG_X{i}'), value)
        self.failure = None
        self.uc.emu_start(self.image.symbols[name], RETURN + 4, timeout=30_000_000, count=80000000)
        if self.failure: raise AssertionError(self.failure)
        pc = self.uc.reg_read(regs.UC_ARM64_REG_PC)
        if pc != RETURN: raise AssertionError(f'{name} did not return: PC={pc:#x}')
        return self.uc.reg_read(regs.UC_ARM64_REG_X0)

    def prepare(self, **kwargs):
        self.uc.mem_write(DTB, fixture(**kwargs))
        return self.call('mm_prepare', DTB, self.image.symbols['_end'])

    def initialize(self, **kwargs):
        assert self.prepare(**kwargs) == 1
        assert self.call('mm_finalize_framebuffer', FRAMEBUFFER, 0x200000) == 1
        self.call('mmu_init', FRAMEBUFFER, 0x200000)
        assert self.sctlr & 5 == 5
        assert self.call('mm_init_allocator') == 1

    def stats(self):
        assert self.call('mm_stats', STATS) == 1
        return struct.unpack('<5Q', self.uc.mem_read(STATS, 40))


class MemoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image = Image(sys.argv[1])

    def setUp(self): self.m = Machine(self.image)

    def test_linked_allocator_and_stack_guard(self):
        bottom = self.image.symbols['__kernel_stack_bottom']
        self.m.uc.mem_write(bottom, b'\xa7' * 64)
        self.m.initialize(dynamic=True)
        before = self.m.stats()
        self.assertEqual(self.m.call('mm_self_test'), 1)
        self.assertEqual(before, self.m.stats())
        self.assertEqual(self.m.uc.mem_read(bottom, 64), b'\xa7' * 64)
        self.assertGreaterEqual(self.m.minimum_sp, bottom + 64)
        p = self.m.call('mm_alloc_pages', 3, 4)
        self.assertNotEqual(p, 0)
        self.assertEqual(p % (4 * 4096), 0)
        self.assertEqual(self.m.call('mm_free_pages', p, 2), 0)
        self.assertEqual(self.m.call('mm_free_pages', p + 4096, 2), 0)
        self.assertEqual(self.m.call('mm_free_pages', p, 3), 1)
        self.assertEqual(self.m.call('mm_free_pages', p, 3), 0)
        a = self.m.call('mm_alloc', 123, 64)
        self.assertNotEqual(a, 0)
        self.assertEqual(a % 64, 0)
        self.assertEqual(self.m.call('mm_free_pages', a, 2048), 0)
        self.assertEqual(self.m.call('mm_free', a + 1), 0)
        self.assertEqual(self.m.call('mm_free', a), 1)
        self.assertEqual(self.m.call('mm_free', a), 0)
        self.assertEqual(before, self.m.stats())

    def test_zeroing_exhaustion_owner_and_reentry(self):
        self.m.initialize()
        a = self.m.call('mm_alloc', 8192, 4096)
        self.m.uc.mem_write(a, b'\xcc' * 8192)
        self.assertEqual(self.m.call('mm_free', a), 1)
        b = self.m.call('mm_zalloc', 8192, 4096)
        self.assertEqual(a, b)
        self.assertEqual(self.m.uc.mem_read(b, 8192), b'\0' * 8192)
        self.assertEqual(self.m.call('mm_alloc', 9 * 1024 * 1024, 1), 0)
        self.assertEqual(self.m.call('mm_alloc_pages', 0xffffffffffffffff, 1), 0)
        self.assertEqual(self.m.call('mm_alloc', 1, 3), 0)
        for attr, value in [('core', 1), ('core', 2), ('core', 3), ('core', 0x10000),
                            ('core', 0x100000000), ('daif', 0), ('level', 8)]:
            old = getattr(self.m, attr)
            setattr(self.m, attr, value)
            self.assertEqual(self.m.call('mm_alloc', 1, 1), 0)
            self.assertEqual(self.m.call('mm_free', b), 0)
            setattr(self.m, attr, old)
        self.m.uc.mem_write(self.image.symbols['busy'], struct.pack('<I', 1))
        self.assertEqual(self.m.call('mm_alloc', 1, 1), 0)
        self.m.uc.mem_write(self.image.symbols['busy'], b'\0' * 4)
        self.assertEqual(self.m.call('mm_free', b), 1)

    def test_renderer_uses_heap_and_reserved_framebuffer(self):
        self.assertEqual(self.m.prepare(), 1)
        self.m.uc.mem_map(0xfe003000, 4096)
        mailbox = self.image.symbols['mbox']
        messages = []

        def firmware(uc, address, size, _):
            request = list(struct.unpack('<36I', uc.mem_read(mailbox, 36 * 4)))
            if request[0] == 35 * 4:
                request[20] = 32
                request[28] = FRAMEBUFFER | 0xc0000000
                request[29] = 800 * 480 * 4 * 3
                request[33] = 800 * 4
                uc.mem_write(mailbox, struct.pack('<36I', *request))
            uc.reg_write(regs.UC_ARM64_REG_X0, 1)
            uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

        def uart(uc, address, size, _):
            pointer = uc.reg_read(regs.UC_ARM64_REG_X0)
            messages.append(bytes(uc.mem_read(pointer, 160)).split(b'\0')[0])
            uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

        for name, callback in [('mbox_call', firmware), ('uart_writeText', uart)]:
            address = self.image.symbols[name]
            self.m.uc.hook_add(UC_HOOK_CODE, callback, begin=address, end=address)
        self.assertEqual(self.m.call('fb_init', 800, 480), FRAMEBUFFER)
        self.assertIn(b'[OK] Page allocator and heap self-test passed\n', messages)
        self.assertEqual(self.m.stats()[4], 800 * 480 * 4)
        pointer = struct.unpack('<Q', self.m.uc.mem_read(self.image.symbols['backBuffer'], 8))[0]
        self.assertEqual(pointer % 64, 0)
        self.assertGreaterEqual(pointer, self.image.symbols['_end'])
        self.assertEqual(self.m.uc.mem_read(pointer, 800 * 480 * 4), b'\0' * (800 * 480 * 4))
        self.m.call('fill_rect', 0, 0, 480, 800, 0x12345678)
        expected = struct.pack('<I', 0x12345678) * (800 * 480)
        self.assertEqual(self.m.uc.mem_read(pointer, len(expected)), expected)
        # Dispatcher is in its serial fallback here; desktop threaded tests
        # independently exercise the same callbacks across all four parts.
        self.m.call('fb_swap')
        self.assertEqual(self.m.uc.mem_read(FRAMEBUFFER + len(expected), len(expected)), expected)
        with self.assertRaisesRegex(AssertionError, 'Kernel halted'):
            self.m.call('fb_init', 800, 480)

    def test_tables_and_boot_gate(self):
        self.m.initialize()
        table = self.image.symbols['l2']
        entries = struct.unpack('<2048Q', self.m.uc.mem_read(table, 2048 * 8))
        self.assertEqual(entries[0x2000000 // 0x200000], 0)
        self.assertEqual(entries[0x8000000 // 0x200000], 0)
        self.assertEqual((entries[FRAMEBUFFER // 0x200000] >> 2) & 7, 2)
        self.assertEqual(entries[0xfc000000 // 0x200000] & 3, 1)
        self.assertEqual(entries[0] & (3 << 6), 3 << 6)
        gate = self.image.symbols['__smp_boot_start']
        self.assertEqual((entries[gate // 0x200000] >> 2) & 7, 0)
        self.assertEqual((entries[gate // 0x200000] >> 6) & 3, 0)
        self.assertEqual(self.m.uc.mem_read(self.image.symbols['secondary_mmu_ready'], 4), b'\x01\0\0\0')

    def test_boot_order_and_failed_discovery(self):
        self.assertEqual(self.m.call('mm_alloc', 1, 1), 0)
        self.assertEqual(self.m.call('mm_init_allocator'), 0)
        self.assertEqual(self.m.call('mm_finalize_framebuffer', FRAMEBUFFER, 0x200000), 0)
        self.assertEqual(self.m.prepare(bad_magic=True), 0)
        self.assertEqual(self.m.prepare(), 0)

    def test_main_discovers_before_releasing_workers(self):
        for valid in [False, True]:
            m = Machine(self.image)
            m.uc.mem_write(DTB, fixture(bad_magic=not valid))
            m.uc.reg_write(regs.UC_ARM64_REG_SP, self.image.symbols['__kernel_stack_top'])
            m.uc.reg_write(regs.UC_ARM64_REG_X0, DTB)
            released, messages = [], []

            def uart(uc, address, size, _):
                if address == self.image.symbols['uart_writeText']:
                    pointer = uc.reg_read(regs.UC_ARM64_REG_X0)
                    messages.append(bytes(uc.mem_read(pointer, 160)).split(b'\0')[0])
                uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

            def release(uc, address, size, _):
                released.append(uc.reg_read(regs.UC_ARM64_REG_X0))
                uc.emu_stop()

            for name in ['uart_init', 'uart_writeText']:
                address = self.image.symbols[name]
                m.uc.hook_add(UC_HOOK_CODE, uart, begin=address, end=address)
            address = self.image.symbols['secondary_core_start']
            m.uc.hook_add(UC_HOOK_CODE, release, begin=address, end=address)
            m.uc.emu_start(self.image.symbols['main'], RETURN, timeout=30000000, count=80000000)
            self.assertEqual(released, [1] if valid else [])
            if valid:
                self.assertIsNone(m.failure)
                self.assertIn(b'[OK] Firmware RAM and reservations discovered\n', messages)
            else:
                self.assertIn('Kernel halted', m.failure)

    def test_reservation_conflicts_and_reinitialization(self):
        self.assertEqual(self.m.prepare(), 1)
        self.assertEqual(self.m.call('mm_prepare', DTB, self.image.symbols['_end']), 0)
        self.assertEqual(self.m.call('mm_finalize_framebuffer', DTB, 4096), 0)
        self.assertEqual(self.m.call('mm_block_mapped', 0), 0)
        self.assertEqual(self.m.call('mm_init_allocator'), 0)
        for no_map in [0, DTB, FRAMEBUFFER]:
            m = Machine(self.image)
            self.assertEqual(m.prepare(no_map=no_map), 1)
            self.assertEqual(m.call('mm_finalize_framebuffer', FRAMEBUFFER, 0x200000), 0)
            self.assertEqual(m.call('mm_init_allocator'), 0)

    def test_cache_state_and_live_table_rebuild(self):
        for state in [1, 4, 5]:
            self.m.sctlr = state
            self.assertEqual(self.m.prepare(), 0)
        self.m.sctlr = 0
        self.m.initialize()
        before = self.m.stats()
        self.assertEqual(self.m.call('mm_init_allocator'), 0)
        table = self.image.symbols['l2']
        descriptors = self.m.uc.mem_read(table, 2048 * 8)
        with self.assertRaisesRegex(AssertionError, 'Kernel halted'):
            self.m.call('mmu_init', FRAMEBUFFER, 0x200000)
        self.assertEqual(self.m.uc.mem_read(table, 2048 * 8), descriptors)
        self.assertEqual(before, self.m.stats())

    def test_primary_preserves_dtb_and_clears_bss(self):
        for level in [4, 8]:
            m = Machine(self.image)
            m.level, m.stop_at_main = level, True
            start, end = self.image.symbols['__bss_start'], self.image.symbols['__bss_end']
            boot_bottom = self.image.symbols['__boot_stack_bottom']
            boot_top = self.image.symbols['__boot_stack_top']
            boot_writes = []
            m.uc.hook_add(UC_HOOK_MEM_WRITE, lambda uc, access, a, s, v, _: boot_writes.append(a),
                          begin=boot_bottom, end=boot_top - 1)
            m.uc.mem_write(boot_bottom, b'\xe9' * 64)
            m.uc.mem_write(start, b'\xaa' * (end - start))
            data = fixture(initrd=(end, end + 4096), dynamic=True)
            m.uc.mem_write(DTB, data)
            m.uc.mem_write(end, b'\xc3' * 4096)
            m.uc.reg_write(regs.UC_ARM64_REG_X0, DTB)
            m.uc.emu_start(self.image.symbols['_start'], RETURN, timeout=30000000, count=80000000)
            self.assertIsNone(m.failure)
            self.assertEqual(m.main_argument, DTB)
            self.assertEqual(m.uc.mem_read(start, end - start), b'\0' * (end - start))
            self.assertEqual(m.uc.mem_read(DTB, len(data)), data)
            self.assertEqual(m.uc.mem_read(end, 4096), b'\xc3' * 4096)
            self.assertEqual(m.uc.mem_read(boot_bottom, 64), b'\xe9' * 64)
            self.assertTrue(boot_writes)
            self.assertGreaterEqual(min(boot_writes), boot_bottom + 64)

    def test_bootstrap_stack_is_part_of_loaded_image(self):
        section = self.image.named['.boot.stack']
        bottom, top = self.image.symbols['__boot_stack_bottom'], self.image.symbols['__boot_stack_top']
        self.assertEqual(section[1], 1)  # PROGBITS, not NOLOAD/NOBITS.
        self.assertEqual(section[2] & 3, 3)  # Writable and allocated.
        self.assertEqual(section[3], bottom)
        self.assertEqual(section[5], top - bottom)
        self.assertEqual(len(self.image.section_bytes(section)), 32768)
        self.assertEqual(bottom % 16, 0)
        self.assertLessEqual(top, self.image.symbols['__kernel_file_end'])
        self.assertLessEqual(self.image.symbols['__kernel_file_end'], self.image.symbols['__bss_start'])

    def rejected_boot(self, pointer=DTB, data=None, payload=None, level=4):
        m = Machine(self.image)
        m.level, m.stop_at_main = level, True
        start, end = self.image.symbols['__bss_start'], self.image.symbols['__bss_end']
        m.uc.mem_write(start, b'\xa5' * (end - start))
        if data is not None:
            m.uc.mem_write(pointer, bytes(data))
        if payload is not None:
            payload_address, contents = payload
            m.uc.mem_write(payload_address, contents)
        before = bytes(m.uc.mem_read(start, end - start))
        writes, rejected = [], []
        m.uc.hook_add(UC_HOOK_MEM_WRITE, lambda uc, access, a, s, v, _: writes.append((a, s)))

        def rejection(uc, address, size, _):
            rejected.append(True)
            uc.emu_stop()

        # On the old image this hook is absent: reaching main demonstrates the bug.
        address = self.image.symbols.get('boot_memory_rejected')
        if address is not None:
            m.uc.hook_add(UC_HOOK_CODE, rejection, begin=address, end=address)
        m.uc.reg_write(regs.UC_ARM64_REG_X0, pointer)
        m.uc.emu_start(self.image.symbols['_start'], RETURN, timeout=30000000, count=80000000)
        self.assertIsNone(m.main_argument, 'unsafe firmware data reached main after BSS clearing')
        self.assertTrue(rejected)
        boot_bottom = self.image.symbols['__boot_stack_bottom']
        boot_top = self.image.symbols['__boot_stack_top']
        self.assertTrue(all(boot_bottom <= a and a + size <= boot_top for a, size in writes),
                        'rejection wrote outside the file-backed bootstrap stack')
        if pointer < end:
            self.assertEqual(writes, [], 'overlapping DTB must be rejected before any stores')
        self.assertEqual(m.uc.mem_read(start, end - start), before)
        if data is not None:
            self.assertEqual(m.uc.mem_read(pointer, len(data)), data)
        if payload is not None:
            self.assertEqual(m.uc.mem_read(payload_address, len(contents)), contents)
        return m

    def test_boot_rejects_overlapping_dtb_without_writes(self):
        start, end = self.image.symbols['__bss_start'], self.image.symbols['_end']
        for level in (4, 8):
            for address in (self.image.symbols['__boot_stack_bottom'], start,
                            self.image.symbols['__kernel_stack_top'] - 256,
                            self.image.symbols['__user_start'],
                            self.image.symbols['__smp_boot_start'], end - 16):
                with self.subTest(level=level, address=hex(address)):
                    self.rejected_boot(address, fixture(), level=level)

    def test_boot_rejects_initrd_conflicts_before_clear(self):
        start, end = self.image.symbols['__bss_start'], self.image.symbols['_end']
        for level in (4, 8):
            for cells in (1, 2):
                for address in (start, self.image.symbols['__kernel_stack_top'] - 128,
                                self.image.symbols['__user_start'],
                                self.image.symbols['__smp_boot_start'], end - 64):
                    with self.subTest(level=level, cells=cells, address=hex(address)):
                        self.rejected_boot(data=fixture(initrd=(address, address + 64),
                                                       initrd_cells=cells),
                                           payload=(address, b'\xc7' * 64), level=level)

    def test_boot_rejects_reserved_payload_conflicts_before_clear(self):
        start = self.image.symbols['__bss_start']
        for options in ({'reservation': (start - 16, 32)}, {'no_map': start}):
            with self.subTest(options=options):
                self.rejected_boot(data=fixture(**options), payload=(start, b'\x93' * 64))

    def test_boot_rejects_bad_firmware_before_clear(self):
        for pointer in (0, 4095, DTB + 1, 0xfc000000 - 32, 0xfffffffffffffff8):
            with self.subTest(pointer=hex(pointer)):
                self.rejected_boot(pointer)
        for data in (fixture(bad_magic=True), fixture(initrd=(0x5000000, None)),
                     fixture(initrd=(0x5001000, 0x5000000)), fixture(ram_size=0x200000)):
            self.rejected_boot(data=data)
        data = bytearray(fixture())
        struct.pack_into('>I', data, 4, 0x200001)
        self.rejected_boot(data=data)


if __name__ == '__main__':
    if len(sys.argv) < 2: raise SystemExit(__doc__)
    unittest.main(argv=[sys.argv[0]] + sys.argv[2:])
