"""Minimal AArch64 encoder for hand-written model test programs.

Only the instruction forms used by the linked-kernel tests are provided.
Registers are numbers; 31 is SP or XZR as the instruction form defines.
"""
import struct

EQ, NE, HS, LO, MI, PL = 0, 1, 2, 3, 4, 5
SP = XZR = 31
FPCR, FPSR, NZCV = (3, 3, 4, 4, 0), (3, 3, 4, 4, 1), (3, 3, 4, 2, 0)


class Program:
    def __init__(self, base=0):
        self.base, self.words, self.labels, self.fixups = base, [], {}, []

    def emit(self, *words):
        self.words.extend(words)
        return self

    @property
    def here(self):
        return self.base + 4 * len(self.words)

    def label(self, name):
        self.labels[name] = len(self.words)
        return self

    def address(self, name):
        return self.base + 4 * self.labels[name]

    def mov(self, rd, value):
        """Load any 64-bit constant with MOVZ/MOVK."""
        value &= 0xffffffffffffffff
        self.emit(0xd2800000 | ((value & 0xffff) << 5) | rd)
        for part in (1, 2, 3):
            chunk = (value >> (16 * part)) & 0xffff
            if chunk:
                self.emit(0xf2800000 | (part << 21) | (chunk << 5) | rd)
        return self

    def mov_reg(self, rd, rm): return self.emit(0xaa0003e0 | (rm << 16) | rd)
    def mov_to_sp(self, rn): return self.emit(0x9100001f | (rn << 5))
    def mov_from_sp(self, rd): return self.emit(0x910003e0 | rd)
    def add(self, rd, rn, immediate): return self.emit(0x91000000 | (immediate << 10) | (rn << 5) | rd)
    def sub(self, rd, rn, immediate): return self.emit(0xd1000000 | (immediate << 10) | (rn << 5) | rd)
    def add_reg(self, rd, rn, rm): return self.emit(0x8b000000 | (rm << 16) | (rn << 5) | rd)
    def sub_reg(self, rd, rn, rm): return self.emit(0xcb000000 | (rm << 16) | (rn << 5) | rd)
    def cmp(self, rn, immediate): return self.emit(0xf100001f | (immediate << 10) | (rn << 5))
    def ldr(self, rt, rn, offset=0): return self.emit(0xf9400000 | ((offset // 8) << 10) | (rn << 5) | rt)
    def str(self, rt, rn, offset=0): return self.emit(0xf9000000 | ((offset // 8) << 10) | (rn << 5) | rt)
    def ldrb(self, rt, rn, offset=0): return self.emit(0x39400000 | (offset << 10) | (rn << 5) | rt)
    def strb(self, rt, rn, offset=0): return self.emit(0x39000000 | (offset << 10) | (rn << 5) | rt)
    def svc(self): return self.emit(0xd4000001)
    def nop(self): return self.emit(0xd503201f)
    def udf(self): return self.emit(0x00000000)
    def ret(self): return self.emit(0xd65f03c0)
    def br(self, rn): return self.emit(0xd61f0000 | (rn << 5))
    def blr(self, rn): return self.emit(0xd63f0000 | (rn << 5))
    def msr(self, register, rt):
        op0, op1, crn, crm, op2 = register
        return self.emit(0xd5000000 | (op0 << 19) | (op1 << 16) | (crn << 12) | (crm << 8) | (op2 << 5) | rt)
    def mrs(self, rt, register):
        self.msr(register, rt)
        self.words[-1] |= 1 << 21
        return self
    def fmov_to_d(self, vd, rn): return self.emit(0x9e670000 | (rn << 5) | vd)
    def fmov_from_d(self, rd, vn): return self.emit(0x9e660000 | (vn << 5) | rd)
    def ins_high(self, vd, rn): return self.emit(0x4e181c00 | (rn << 5) | vd)
    def umov_high(self, rd, vn): return self.emit(0x4e183c00 | (vn << 5) | rd)

    def _branch(self, word, target, shift, bits):
        self.fixups.append((len(self.words), target, shift, bits))
        return self.emit(word)

    def b(self, target): return self._branch(0x14000000, target, 0, 26)
    def bl(self, target): return self._branch(0x94000000, target, 0, 26)
    def b_cond(self, condition, target): return self._branch(0x54000000 | condition, target, 5, 19)
    def cbz(self, rt, target): return self._branch(0xb4000000 | rt, target, 5, 19)
    def cbnz(self, rt, target): return self._branch(0xb5000000 | rt, target, 5, 19)

    def syscall(self, number, argument=None):
        """x8 = number, optional constant x0, then SVC."""
        if argument is not None:
            self.mov(0, argument)
        return self.mov(8, number).svc()

    def code(self):
        words = list(self.words)
        for index, target, shift, bits in self.fixups:
            delta = self.labels[target] - index
            if not -(1 << (bits - 1)) <= delta < (1 << (bits - 1)):
                raise ValueError('branch out of range: ' + target)
            words[index] |= (delta & ((1 << bits) - 1)) << shift
        return struct.pack(f'<{len(words)}I', *words)
