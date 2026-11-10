"""Linked-kernel model with native privilege levels, system registers and MMU.

Unlike the scripted Machine in memory_management.py, Unicorn executes EL0 and
EL1 code, translation-table walks, permission checks and exception returns
itself. This module models what Unicorn does not: exception entry to the EL1
vectors, four CPU contexts over shared RAM, the GIC-400 CPU interface and the
per-core virtual timer. Time is counted in executed-instruction budgets.
It is not a Pi, cache-coherency, firmware or timing validation.
"""
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64_const import ARM64_OP_MEM
from unicorn import (UcError, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE,
                     UC_ERR_EXCEPTION, UC_ERR_INSN_INVALID)
from unicorn import arm64_const as regs
from unicorn.unicorn_const import UC_CTL_TLB_FLUSH

from memory_management import Machine, FRAMEBUFFER

# Unused low RAM inside the kernel's EL1-executable first block.
LOW = 0x10000
RETURN = LOW
WORKER_RETURN = LOW + 0x40
KERNEL_CODE = 0x20000
SCRATCH = 0x7c00000
STATUS = 0x7bff000

GIC_DISTRIBUTOR, GIC_CPU = 0xff841000, 0xff842000
GICD_CTLR, GICD_ISENABLER0, GICD_IPRIORITYR = GIC_DISTRIBUTOR, GIC_DISTRIBUTOR + 0x100, GIC_DISTRIBUTOR + 0x400
GICC_CTLR, GICC_PMR, GICC_IAR, GICC_EOIR = GIC_CPU, GIC_CPU + 4, GIC_CPU + 0xc, GIC_CPU + 0x10
TIMER_IRQ, SPURIOUS = 27, 1023
COUNTER_HZ = 54000000

USER_BASE, USER_END, PAGE = 0x100000000, 0x140000000, 4096
IMAGE_MAGIC = 0x31676d4945544e56
HEADER_BYTES = 48
PROGRAM_BASE = USER_BASE + HEADER_BYTES
FAULT_DATA, FAULT_INSTRUCTION, FAULT_UNDEFINED, FAULT_ADDRESS, FAULT_OTHER = -1001, -1002, -1003, -1004, -1005
OK, INVALID, PENDING = 1, 0, -1

EXCP_UDEF, EXCP_SWI, EXCP_PREFETCH_ABORT, EXCP_DATA_ABORT = 1, 2, 3, 4
EL1H_MASKED = 0x3c5
IRQ_MASK = 0x80

SCR_EL3, HCR_EL2, SCTLR_EL1 = (1, 1, 3, 6, 0), (1, 1, 3, 4, 0), (1, 0, 3, 0, 0)
X = [getattr(regs, f'UC_ARM64_REG_X{i}') for i in range(29)] + [regs.UC_ARM64_REG_X29, regs.UC_ARM64_REG_X30]
V = [getattr(regs, f'UC_ARM64_REG_V{i}') for i in range(32)]


def signed(value):
    return (value & 0xffffffff) - ((value & 0x80000000) << 1)


def image(text, rodata=b'', data=b'', bss=0, entry=PROGRAM_BASE):
    """Flat program image: text is assembled for PROGRAM_BASE."""
    pad = lambda blob: blob + bytes(-len(blob) % PAGE)
    text = pad(bytes(HEADER_BYTES) + text)
    rodata = pad(rodata)
    header = struct.pack('<6Q', IMAGE_MAGIC, entry, len(text), len(rodata), len(data), bss)
    return header + text[HEADER_BYTES:] + rodata + data


def data_base(text, rodata=b''):
    """Address of the writable data of an image built from these parts."""
    return USER_BASE + len(text) + HEADER_BYTES + (-(len(text) + HEADER_BYTES) % PAGE) + len(rodata) + (-len(rodata) % PAGE)


def register(name):
    return getattr(regs, 'UC_ARM64_REG_' + name.upper())


class Core:
    def __init__(self, index):
        self.index = index
        self.context = None
        self.armed = False
        self.remaining = 0
        self.forced_irq = None
        self.idle = False
        self.timer_log = []
        self.last_pc = 0


class CpuMachine(Machine):
    def __init__(self, image, slice_instructions=4000):
        super().__init__(image)
        uc = self.uc
        # As after the firmware stub: EL2 and EL1 are non-secure AArch64.
        uc.reg_write(regs.UC_ARM64_REG_CP_REG, SCR_EL3 + (0x501,))
        uc.reg_write(regs.UC_ARM64_REG_CP_REG, HCR_EL2 + (1 << 31,))
        uc.reg_write(regs.UC_ARM64_REG_VBAR_EL1, image.symbols['vector_table'])
        uc.mem_map(0xff840000, 0x10000)
        uc.mem_map(0xfe003000, 0x1000)
        self.cores = [Core(i) for i in range(4)]
        self.slice_instructions = slice_instructions
        self.exception = None
        self.last_stop = None
        self.stale_code = False
        self.entries = []          # (core, kind, origin EL, interrupted PC)
        self.interrupted = []      # (core, ASID installed, interrupted PC) of each IRQ
        self.gic_writes = []       # (core, register, value)
        self.completed = []        # (core, EOIR value)
        self.maintenance = []      # (core, mnemonic, operand, register value)
        self.syscalls = []         # (core, ASID of the caller, number, argument)
        self.expired_syscalls = [] # (core, number) of calls entered after the slice had expired
        self.gic_group0 = False
        self.clock_us = 0
        self.lock_version = 0
        self.reservations = [None] * 4
        self.decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        self.decoder.detail = True
        uc.hook_add(UC_HOOK_INTR, self.interrupt)
        uc.hook_add(UC_HOOK_MEM_READ, self.gic_read, begin=GIC_DISTRIBUTOR, end=GIC_CPU + 0xfff)
        uc.hook_add(UC_HOOK_MEM_WRITE, self.gic_write, begin=GIC_DISTRIBUTOR, end=GIC_CPU + 0xfff)
        uc.hook_add(UC_HOOK_MEM_READ, self.system_timer, begin=0xfe003004, end=0xfe003007)
        uc.hook_add(UC_HOOK_CODE, self.worker_return, begin=WORKER_RETURN, end=WORKER_RETURN)
        lock = image.symbols['scheduler_lock']
        uc.hook_add(UC_HOOK_MEM_WRITE, self.lock_write, begin=lock, end=lock + 3)
        uc.hook_add(UC_HOOK_CODE, self.syscall, begin=image.symbols['handle_sys_call'],
                    end=image.symbols['handle_sys_call'])
        for name in ('task_port_halt', 'parallel_port_halt'):
            uc.hook_add(UC_HOOK_CODE, self.halted, begin=image.symbols[name], end=image.symbols[name])
        scanner = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        scanner.skipdata = True
        text = image.named['.text']
        for ins in scanner.disasm(image.section_bytes(text), text[3]):
            if ins.mnemonic == 'wfe':
                uc.hook_add(UC_HOOK_CODE, self.wait, begin=ins.address, end=ins.address)
            elif ins.mnemonic in ('ldaxr', 'stxr'):
                uc.hook_add(UC_HOOK_CODE, self.exclusive, user_data=(ins.mnemonic, ins.op_str),
                            begin=ins.address, end=ins.address)

    # --- CPU contexts -----------------------------------------------------
    @property
    def cpu(self):
        return self.cores[self.core]

    def select(self, core):
        """Make another modeled CPU current; translations are per CPU."""
        if core == self.core:
            return
        self.cpu.context = self.uc.context_save()
        target = self.cores[core]
        if target.context is None:
            # A secondary starts from the common EL1 reset state.
            for name in ('SP', 'SP_EL0', 'ELR_EL1', 'FPCR', 'FPSR'):
                self.uc.reg_write(register(name), 0)
            self.uc.reg_write(regs.UC_ARM64_REG_CP_REG, SCTLR_EL1 + (0x30d00800,))
            self.uc.reg_write(regs.UC_ARM64_REG_TTBR0_EL1, 0)
            self.set_pstate(EL1H_MASKED)
        else:
            self.uc.context_restore(target.context)
        self.core = core
        self.uc._Uc__ctl_w(UC_CTL_TLB_FLUSH)

    def set_pstate(self, value):
        self.uc.reg_write(regs.UC_ARM64_REG_PSTATE, value)
        # Unicorn recomputes its cached exception level on a control-register write.
        self.uc.reg_write(regs.UC_ARM64_REG_CP_REG, SCTLR_EL1 + (self.system(SCTLR_EL1),))

    def stack_top(self):
        return self.image.symbols['__kernel_stack_top' if self.core == 0 else f'__core{self.core}_stack_top']

    @property
    def pstate(self):
        return self.uc.reg_read(regs.UC_ARM64_REG_PSTATE)

    @property
    def level(self):
        return (self.uc.reg_read(regs.UC_ARM64_REG_PSTATE) >> 2) & 3

    @level.setter
    def level(self, value):
        pass  # The base class initializes its scripted exception level.

    @property
    def pc(self):
        return self.uc.reg_read(regs.UC_ARM64_REG_PC)

    def system(self, key):
        return self.uc.reg_read(regs.UC_ARM64_REG_CP_REG, key)

    def state(self):
        """Every register an interrupt must preserve."""
        uc = self.uc
        values = {f'x{i}': uc.reg_read(X[i]) for i in range(31)}
        values.update({f'v{i}': uc.reg_read(V[i]) for i in range(32)})
        values.update(sp=uc.reg_read(regs.UC_ARM64_REG_SP), pc=self.pc, pstate=self.pstate,
                      fpcr=uc.reg_read(regs.UC_ARM64_REG_FPCR), fpsr=uc.reg_read(regs.UC_ARM64_REG_FPSR))
        return values

    # --- Modeled devices and instructions ---------------------------------
    def syscall(self, uc, address, size, _):
        asid = uc.reg_read(regs.UC_ARM64_REG_TTBR0_EL1) >> 48
        self.syscalls.append((self.core, asid, uc.reg_read(X[0]), uc.reg_read(X[1])))
        if self.cpu.armed and self.cpu.remaining <= 0:
            self.expired_syscalls.append((self.core, uc.reg_read(X[0])))

    def halted(self, uc, address, size, _):
        pointer = uc.reg_read(regs.UC_ARM64_REG_X0)
        text = bytes(uc.mem_read(pointer, 96)).split(b'\0')[0].decode('ascii', 'replace').strip()
        self.failure = f'Kernel halted on core {self.core}: {text}'
        uc.emu_stop()

    def privileged(self, uc, address, size, instruction):
        mnemonic, operands = instruction
        parts = [part.strip().lower() for part in operands.split(',')]
        cpu = self.cpu
        if mnemonic == 'mrs' and parts[1] == 'mpidr_el1':
            uc.reg_write(register(parts[0]), 0x80000000 | self.core)
        elif mnemonic == 'mrs' and parts[1] == 'cntfrq_el0':
            uc.reg_write(register(parts[0]), COUNTER_HZ)
        elif mnemonic == 'msr' and parts[0] == 'cntv_tval_el0':
            cpu.timer_log.append(('interval', uc.reg_read(register(parts[1]))))
        elif mnemonic == 'msr' and parts[0] == 'cntv_ctl_el0':
            value = uc.reg_read(register(parts[1]))
            cpu.armed = value & 3 == 1
            cpu.remaining = self.slice_instructions
            cpu.timer_log.append(('start' if cpu.armed else 'stop', value))
        elif mnemonic == 'ic':
            # Like an instruction cache, Unicorn keeps executing code it has
            # already translated until the kernel invalidates it.
            self.maintenance.append((self.core, mnemonic, parts[0], None))
            self.stale_code = True
            uc.emu_stop()
        else:
            if mnemonic in ('tlbi', 'dc'):
                value = uc.reg_read(register(parts[1])) if len(parts) > 1 else None
                self.maintenance.append((self.core, mnemonic, parts[0], value))
            return  # Executed by the CPU model itself.
        uc.reg_write(regs.UC_ARM64_REG_PC, address + size)

    def wait(self, uc, address, size, _):
        # WFE: end this CPU's quantum; another CPU's SEV is not modeled.
        self.cpu.idle = True
        uc.reg_write(regs.UC_ARM64_REG_PC, address + size)
        uc.emu_stop()

    def lock_write(self, uc, access, address, size, value, _):
        self.lock_version += 1

    def exclusive(self, uc, address, size, instruction):
        # One Unicorn CPU provides one exclusive monitor: model one per CPU.
        mnemonic, operands = instruction
        parts = operands.split(', ')
        pointer = uc.reg_read(register(parts[-1].strip('[]')))
        assert pointer == self.image.symbols['scheduler_lock']
        if mnemonic == 'ldaxr':
            held = struct.unpack('<I', uc.mem_read(pointer, 4))[0]
            uc.reg_write(register(parts[0]), held)
            self.reservations[self.core] = self.lock_version
            if held:
                # Let the modeled owner run instead of spinning out this quantum.
                self.cpu.idle = True
                uc.emu_stop()
        else:
            success = self.reservations[self.core] == self.lock_version
            if success:
                uc.mem_write(pointer, struct.pack('<I', uc.reg_read(register(parts[1])) & 0xffffffff))
                self.lock_version += 1
            self.reservations[self.core] = None
            uc.reg_write(register(parts[0]), 0 if success else 1)
        uc.reg_write(regs.UC_ARM64_REG_PC, address + size)

    def system_timer(self, uc, access, address, size, value, _):
        self.clock_us += 1000
        uc.mem_write(0xfe003004, struct.pack('<I', self.clock_us & 0xffffffff))

    def pending_irq(self):
        cpu = self.cpu
        if cpu.forced_irq is not None:
            return cpu.forced_irq
        return TIMER_IRQ if cpu.armed and cpu.remaining <= 0 else None

    def gic_read(self, uc, access, address, size, value, _):
        if address == GICC_IAR:
            pending = self.pending_irq()
            uc.mem_write(address, struct.pack('<I', SPURIOUS if pending is None else pending))
        elif address == GICD_ISENABLER0 and self.gic_group0:
            uc.mem_write(address, struct.pack('<I', 0))

    def gic_write(self, uc, access, address, size, value, _):
        self.gic_writes.append((self.core, address, value))
        if address == GICC_EOIR:
            self.completed.append((self.core, value))
            self.cpu.forced_irq = None

    def worker_return(self, uc, address, size, _):
        uc.reg_write(regs.UC_ARM64_REG_X0, self.core)
        uc.reg_write(regs.UC_ARM64_REG_X30, WORKER_RETURN)
        uc.reg_write(regs.UC_ARM64_REG_PC, self.image.symbols['parallel_worker_poll'])

    def stub(self, name, callback=None, result=0):
        """Replace a kernel function with a device model returning its result."""
        def run(uc, address, size, _):
            value = result
            if callback is not None:
                value = callback(*[uc.reg_read(X[i]) for i in range(4)])
            uc.reg_write(regs.UC_ARM64_REG_X0, int(value or 0) & 0xffffffffffffffff)
            uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))
        address = self.image.symbols[name]
        self.uc.hook_add(UC_HOOK_CODE, run, begin=address, end=address)

    # --- Processes --------------------------------------------------------
    def create_process(self, blob, affinity=1):
        # The kernel copies the image; the staging buffer is reusable.
        self.uc.mem_write(SCRATCH, blob)
        return self.call('process_create', SCRATCH, len(blob), affinity)

    def process_field(self, process, offset, size=8):
        slot = (process & 63) - 1
        data = self.uc.mem_read(self.image.symbols['processes'] + slot * self.image.sizes['processes'] // 8 + offset, size)
        return int.from_bytes(data, 'little')

    def process_root(self, process):
        return self.process_field(process, 40)

    def translate(self, root, address):
        """Independent walk of a process's tables: (physical address, descriptor)."""
        word = lambda a: struct.unpack('<Q', self.uc.mem_read(a, 8))[0]
        table = root & 0x0000fffffffff000
        for shift in (30, 21, 12):
            entry = word(table + ((address >> shift) & 511) * 8)
            if not entry & 1 or (shift != 12 and entry & 3 != 3):
                return None, entry
            table = entry & 0x0000fffffffff000
        return table + (address & 0xfff), entry

    def user_read(self, process, address, size):
        physical, _ = self.translate(self.process_root(process), address)
        assert physical is not None, f'{address:#x} is not mapped'
        return bytes(self.uc.mem_read(physical, size))

    def reap(self, process):
        """(result, status) of process_reap."""
        result = signed(self.call('process_reap', process, STATUS))
        return result, struct.unpack('<i', self.uc.mem_read(STATUS, 4))[0]

    def free_pages(self):
        assert self.call('mm_stats', STATUS + 64) == 1
        return struct.unpack('<5Q', self.uc.mem_read(STATUS + 64, 40))[2]

    # --- Exception entry --------------------------------------------------
    def interrupt(self, uc, number, _):
        self.exception = number
        uc.emu_stop()

    def fault_address(self, pc):
        """Effective address of the faulting data access, from its operands."""
        physical = pc
        if pc >= USER_BASE:
            physical, _ = self.translate(self.uc.reg_read(regs.UC_ARM64_REG_TTBR0_EL1), pc)
        ins = next(self.decoder.disasm(bytes(self.uc.mem_read(physical, 4)), pc))
        for operand in ins.operands:
            if operand.type == ARM64_OP_MEM:
                address = self.uc.reg_read(self.capstone_register(ins, operand.mem.base))
                if ins.writeback and len(ins.operands) > 2 and ins.operands[-1].type != ARM64_OP_MEM:
                    return address, ins  # Post-indexed: the base is the address.
                if operand.mem.index:
                    address += self.uc.reg_read(self.capstone_register(ins, operand.mem.index))
                return (address + operand.mem.disp) & 0xffffffffffffffff, ins
        raise AssertionError(f'cannot model fault address for {ins.mnemonic} {ins.op_str}')

    def capstone_register(self, ins, number):
        name = ins.reg_name(number)
        return regs.UC_ARM64_REG_SP if name == 'sp' else register(name)

    def take(self, kind, syndrome=None, far=None):
        """Architectural AArch64 exception entry to EL1 using SP_EL1."""
        uc = self.uc
        pstate, pc = self.pstate, self.pc
        origin = (pstate >> 2) & 3
        if origin not in (0, 1) or (origin == 1 and not pstate & 1):
            raise AssertionError(f'exception from unsupported state {pstate:#x}')
        if origin == 1 and kind == 'sync':
            raise AssertionError(f'kernel fault on core {self.core} at {pc:#x} (syndrome {syndrome:#x})')
        self.entries.append((self.core, kind, origin, pc))
        if kind == 'irq':
            self.interrupted.append((self.core, uc.reg_read(regs.UC_ARM64_REG_TTBR0_EL1) >> 48, pc))
        uc.reg_write(regs.UC_ARM64_REG_ELR_EL1, pc)
        uc.reg_write(regs.UC_ARM64_REG_CP_REG, (4, 0, 3, 0, 0, pstate))  # SPSR_EL1
        if syndrome is not None:
            uc.reg_write(regs.UC_ARM64_REG_ESR_EL1, syndrome)
        if far is not None:
            uc.reg_write(regs.UC_ARM64_REG_FAR_EL1, far)
        if origin == 0:
            kernel_sp = uc.reg_read(regs.UC_ARM64_REG_SP_EL1)
            uc.reg_write(regs.UC_ARM64_REG_SP_EL0, uc.reg_read(regs.UC_ARM64_REG_SP))
            self.set_pstate(EL1H_MASKED)
            uc.reg_write(regs.UC_ARM64_REG_SP, kernel_sp)
        else:
            self.set_pstate(EL1H_MASKED)
        offset = (0x400 if origin == 0 else 0x200) + (0x80 if kind == 'irq' else 0)
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_VBAR_EL1) + offset)

    def take_synchronous(self, number):
        lower = self.level == 0
        pc = self.pc
        if number == EXCP_SWI:
            self.take('sync', (0x15 << 26) | (1 << 25))
        elif number == EXCP_UDEF:
            self.take('sync', 1 << 25)
        elif number == EXCP_PREFETCH_ABORT:
            self.take('sync', ((0x20 if lower else 0x21) << 26) | (1 << 25) | 0x07, pc)
        elif number == EXCP_DATA_ABORT:
            address, ins = self.fault_address(pc)
            write = ins.mnemonic.startswith('st')
            self.take('sync', ((0x24 if lower else 0x25) << 26) | (1 << 25) | (write << 6) | 0x07, address)
        else:
            raise AssertionError(f'unmodeled CPU exception {number} at {pc:#x}')

    def deliver_irq(self):
        if self.pending_irq() is not None and not self.pstate & IRQ_MASK:
            self.take('irq')
            return True
        return False

    # --- Execution --------------------------------------------------------
    def chunk(self, budget):
        cpu = self.cpu
        # Short steps only matter while a timer interrupt may become deliverable.
        size = min(budget, 256 if cpu.armed else 16384)
        if cpu.armed and cpu.remaining > 0:
            size = min(size, cpu.remaining)
        return size

    def advance(self, budget, stop=RETURN, counted=True):
        """Run the current CPU for an instruction budget, entering exceptions.

        Uncounted execution is much faster; it is used for single-CPU kernel
        calls while no interrupt can become deliverable."""
        cpu = self.cpu
        cpu.idle = False
        if stop != self.last_stop:
            # Unicorn compiles the stop address into its translated blocks.
            self.uc.ctl_flush_tb()
            self.last_stop = stop
        while budget > 0 and self.pc != stop and not cpu.idle:
            self.deliver_irq()
            count = self.chunk(budget)
            self.exception = None
            pc = self.pc
            try:
                if counted or cpu.armed or cpu.forced_irq is not None:
                    self.uc.emu_start(pc, stop, count=count)
                else:
                    self.uc.emu_start(pc, stop, timeout=120_000_000)
            except UcError as error:
                if error.errno not in (UC_ERR_EXCEPTION, UC_ERR_INSN_INVALID):
                    raise AssertionError(f'core {self.core}: {error} near {self.pc:#x}, '
                                         f'x30={self.uc.reg_read(X[30]):#x}') from error
                self.exception = EXCP_UDEF
            if self.failure:
                raise AssertionError(self.failure)
            if self.stale_code:
                self.uc.ctl_flush_tb()
                self.stale_code = False
            budget -= count
            if cpu.armed:
                cpu.remaining -= count
            if self.exception is not None:
                self.take_synchronous(self.exception)
        cpu.last_pc = self.pc
        return self.pc == stop

    def lock_free(self):
        return not struct.unpack('<I', self.uc.mem_read(self.image.symbols['scheduler_lock'], 4))[0]

    def interject(self, name, *args):
        """Call a kernel function from an interrupted run, on a spare stack,
        and resume the interrupted CPU state afterwards."""
        assert self.lock_free(), 'the interrupted CPUs are inside the scheduler'
        saved, parked = self.uc.context_save(), self.cpu.idle
        self.set_pstate(EL1H_MASKED)
        try:
            return self.call(name, *args, stack=SCRATCH + 0x200000)
        finally:
            self.uc.context_restore(saved)
            self.uc._Uc__ctl_w(UC_CTL_TLB_FLUSH)
            self.cpu.idle = parked

    def call(self, name, *args, budget=400_000_000, stack=None):
        uc = self.uc
        uc.reg_write(regs.UC_ARM64_REG_SP, stack or self.stack_top())
        uc.reg_write(regs.UC_ARM64_REG_X30, RETURN)
        for i, value in enumerate(args):
            uc.reg_write(X[i], value & 0xffffffffffffffff)
        uc.reg_write(regs.UC_ARM64_REG_PC, self.image.symbols[name] if isinstance(name, str) else name)
        self.failure = None
        while budget > 0 and not self.advance(min(budget, 1 << 20), counted=False):
            budget -= 1 << 20
        if self.pc != RETURN:
            raise AssertionError(f'{name} did not return: PC={self.pc:#x}')
        return uc.reg_read(regs.UC_ARM64_REG_X0)

    def initialize(self, **kwargs):
        assert self.prepare(**kwargs) == 1
        assert self.call('mm_finalize_framebuffer', FRAMEBUFFER, 0x200000) == 1
        self.call('mmu_init', FRAMEBUFFER, 0x200000)
        assert self.system(SCTLR_EL1) & 5 == 5
        assert self.call('mm_init_allocator') == 1

    def boot(self, cores=4, **kwargs):
        """Core 0 memory/MMU setup, then each CPU's shared MMU and interrupt setup."""
        self.initialize(**kwargs)
        assert self.call('irq_init_core') == 1
        for core in range(1, cores):
            self.select(core)
            self.uc.reg_write(regs.UC_ARM64_REG_CPACR_EL1, 3 << 20)
            self.uc.reg_write(regs.UC_ARM64_REG_VBAR_EL1, self.image.symbols['vector_table'])
            self.call('mmu_enable_shared')
            assert self.call('irq_init_core') == 1
        self.select(0)
        if cores == 4:
            self.uc.mem_write(self.image.symbols['enabled'], struct.pack('<I', 1))
        assert self.call('task_init') == 1

    def start(self, entry, *args):
        """Core 0 enters a kernel function; the secondaries poll for work."""
        for core in range(4):
            self.select(core)
            self.uc.reg_write(regs.UC_ARM64_REG_SP, self.stack_top())
            self.uc.reg_write(regs.UC_ARM64_REG_X0, core)
            self.uc.reg_write(regs.UC_ARM64_REG_X30, RETURN if core == 0 else WORKER_RETURN)
            if core == 0:
                for i, value in enumerate(args):
                    self.uc.reg_write(X[i], value & 0xffffffffffffffff)
            self.uc.reg_write(regs.UC_ARM64_REG_PC,
                              self.image.symbols[entry if core == 0 else 'parallel_worker_poll'])
        self.select(0)

    def owners(self):
        return struct.unpack('<4Q', self.uc.mem_read(self.image.symbols['current'], 32))

    def run(self, until=None, quantum=307, rounds=20000, cores=4):
        """Interleave the CPUs until core 0 returns or the predicate holds."""
        for round_number in range(rounds):
            for core in range(cores):
                self.select(core)
                self.advance(quantum + (round_number * 13 + core * 7) % 31)
                if core == 0 and self.pc == RETURN:
                    assert self.owners() == (0, 0, 0, 0), 'coordinator returned with a task still owned'
                    return self.uc.reg_read(regs.UC_ARM64_REG_X0)
                if not struct.unpack('<I', self.uc.mem_read(self.image.symbols['scheduler_lock'], 4))[0]:
                    owners = [owner for owner in self.owners() if owner]
                    assert len(owners) == len(set(owners)), 'task owned by two cores'
            if until is not None and until():
                self.select(0)
                return None
        raise AssertionError('modeled CPUs did not finish: ' + repr([hex(cpu.last_pc) for cpu in self.cores]))
