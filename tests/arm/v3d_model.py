"""Scripted Pi 4 V3D 4.2 register model shared by the linked ARM suites.

It answers the firmware power/clock properties, the reset line and AXI
bridges, identity and MMU registers, and executes the driver's clear/present
control lists through the page table snapshot taken at the last TLB flush.
This checks linked driver logic only; it is not V3D hardware behaviour.
"""
import struct

HUB, CORE = 0xfec00000, 0xfec04000
PM_GRAFX, ASB_S, ASB_M = 0xfe10010c, 0xfec11008, 0xfec1100c
GPU_BASE, TARGET_BASE = 0x1000000, 0x1400000
SURFACE = 0x40000
RESET_RELEASED = 64


class V3dDevice:
    def __init__(self, read, write):
        """read(address, size) and write(address, data) access physical RAM."""
        self.mem_read, self.mem_write = read, write
        self.exists = True           # False: nothing answers the identity registers.
        self.domain_missing = False  # Firmware reports no such power domain.
        self.stall = False           # Jobs never complete.
        self.corrupt = False         # One stored pixel is wrong.
        self.bridge_stuck = False    # Bridge acknowledgements never change.
        self.domain = self.clock = 0
        self.pm, self.bridge = 0, {ASB_S: 3, ASB_M: 3}
        self.bin_jobs = self.render_jobs = self.resets = self.flushes = 0
        self.accesses = self.properties = 0
        self.presented = []
        self.reset()

    def reset(self):
        self.mmu_ctl = self.mmuc = self.table = self.illegal = self.gmp = 0
        self.bfc = self.rfc = 255
        self.queue, self.tlb, self.busy = {}, None, False

    def powered(self):
        return bool(self.domain and self.pm & RESET_RELEASED and
                    not self.bridge[ASB_S] & 3 and not self.bridge[ASB_M] & 3)

    def off(self):
        return (not self.domain and not self.pm & RESET_RELEASED and
                self.bridge == {ASB_S: 3, ASB_M: 3})

    def property(self, tag, values):
        self.properties += 1
        assert len(values) == 2
        if tag == 0x38030:
            assert values[0] == 11 and values[1] in (0, 1)
            if self.domain_missing and values[1]: values[1] = 2
            else: self.domain = values[1]
        elif tag == 0x38001:
            assert values == [5, 1] and self.domain
            self.clock = 1
        elif tag == 0x30002:
            assert values[0] == 5
            values[1] = 500000000
        else:
            raise AssertionError(f'unexpected property tag {tag:#x}')
        return values

    def read(self, address):
        self.accesses += 1
        if address == PM_GRAFX: return self.pm
        if address in self.bridge: return self.bridge[address]
        if not self.powered(): return 0
        ident = {CORE: 0x04443356, CORE + 4: 2, HUB + 0xc: 0x124, HUB + 0x10: 0x100}
        if address in ident: return ident[address] if self.exists else 0
        fixed = {CORE + 0x900: 0, HUB + 0x400: 0, HUB + 0x1200: self.mmu_ctl,
                 HUB + 0x1000: self.mmuc & 1, HUB + 0x1204: self.table,
                 HUB + 0x50: 0, CORE + 0x50: 0, CORE + 0x30: 0, CORE + 0x800: 0,
                 CORE + 0x134: self.bfc, CORE + 0x138: self.rfc,
                 CORE + 0xf20: 0x7000}  # Idle-named bits are not faults.
        if address not in fixed: raise AssertionError(f'unexpected V3D read {address:#x}')
        return fixed[address]

    def write(self, address, value):
        self.accesses += 1
        if address == PM_GRAFX:
            assert value >> 24 == 0x5a
            value &= 0xffffff
            if self.pm & RESET_RELEASED and not value & RESET_RELEASED:
                self.reset()
                self.resets += 1
            self.pm = value
            return
        if address in self.bridge:
            assert value >> 24 == 0x5a
            stop = value & 1
            ack = (self.bridge[address] >> 1) & 1 if self.bridge_stuck else stop
            self.bridge[address] = stop | ack << 1
            return
        assert self.powered(), f'write to unpowered V3D register {address:#x}'
        if self.busy:  # Only the shutdown sequence may touch a running job.
            assert address in (CORE + 0x60, HUB + 0x60, CORE + 0x804), hex(address)
        if address in (CORE + 0x60, HUB + 0x60, CORE + 0x58, HUB + 0x58): assert value == 0xffffffff
        elif address == CORE + 0x34: assert value == 0
        elif address == CORE + 0x38: assert value == 0xffffffff
        elif address == HUB + 0x1204: self.table = value
        elif address == HUB + 0x1200:
            self.mmu_ctl = value & ~0x84
            if value & 4: self.flush()
        elif address == HUB + 0x1230: self.illegal = value
        elif address == HUB + 0x1000: self.mmuc = value
        elif address == CORE + 0x30: assert value == 1
        elif address == CORE + 0x24: assert value == 0x0f0f0f0f
        elif address in (CORE + 0x170, CORE + 0x174, CORE + 0x15c, CORE + 0x160, CORE + 0x164):
            self.queue[address] = value
        elif address == CORE + 0x168: self.bin(value)
        elif address == CORE + 0x16c: self.render(value)
        elif address == CORE + 0x804:
            self.gmp = value
            if value & 2: self.busy = False
        else:
            raise AssertionError(f'unexpected V3D write {address:#x}')

    def flush(self):
        assert self.mmu_ctl & 1 and not self.busy
        first = GPU_BASE >> 12
        self.tlb = struct.unpack('<2048I', self.mem_read((self.table << 12) + first * 4, 2048 * 4))
        self.flushes += 1

    def target_mapped(self):
        """Present-window entries in the page table and in the TLB snapshot."""
        first = TARGET_BASE >> 12
        table = struct.unpack('<512I', self.mem_read((self.table << 12) + first * 4, 512 * 4))
        return any(table) or any(self.tlb[(TARGET_BASE - GPU_BASE) >> 12:][:512])

    def translate(self, address):
        entry = self.tlb[(address - GPU_BASE) >> 12] if GPU_BASE <= address < GPU_BASE + 0x800000 else 0
        assert entry & 0x30000000 == 0x30000000, f'V3D access to unmapped {address:#x}'
        return ((entry & 0xffffff) << 12) | (address & 4095)

    def gpu_read(self, address, size):
        data = bytearray()
        while size:
            chunk = min(size, 4096 - (address & 4095))
            data += self.mem_read(self.translate(address), chunk)
            address, size = address + chunk, size - chunk
        return bytes(data)

    def gpu_write(self, address, data):
        while data:
            chunk = min(len(data), 4096 - (address & 4095))
            self.mem_write(self.translate(address), bytes(data[:chunk]))
            address, data = address + chunk, data[chunk:]

    def submit(self):
        assert not self.busy and self.mmu_ctl & 1 and self.mmuc & 1 and not self.gmp & 2
        first = GPU_BASE >> 12
        table = struct.unpack('<2048I', self.mem_read((self.table << 12) + first * 4, 2048 * 4))
        assert table == self.tlb, 'page table changed without a TLB flush'
        assert self.illegal >> 31

    def bin(self, end):
        self.submit()
        start = self.queue[CORE + 0x160]
        data = self.gpu_read(start, end - start)
        assert start == GPU_BASE and data[0] == 120 and data[-2:] == b'\x06\x04'
        assert self.queue[CORE + 0x170] == GPU_BASE + 0x10000 and self.queue[CORE + 0x174] == 0x10000
        assert self.queue[CORE + 0x15c] == GPU_BASE + 0x20002
        self.size = (struct.unpack_from('<H', data, 5)[0] + 1, struct.unpack_from('<H', data, 7)[0] + 1)
        self.bin_jobs += 1
        if self.stall: self.busy = True
        else: self.bfc = (self.bfc + 1) & 255

    def render(self, end):
        self.submit()
        start = self.queue[CORE + 0x164]
        data = self.gpu_read(start, end - start)
        width, height = struct.unpack_from('<HH', data, 2)
        assert start == GPU_BASE + 0x1000 and data[0] == 121 and data[-1] == 13
        assert (width, height) == self.size
        color = data[11:15]
        tiles = ((width + 63) // 64) * ((height + 63) // 64)
        generic = len(data) - 1 - tiles * 3 - 9
        assert data[generic] == 20 and data[generic + 9::3][:tiles] == bytes([23]) * tiles
        tile_start, tile_end = struct.unpack_from('<II', data, generic + 1)
        tile = self.gpu_read(tile_start, tile_end - tile_start)
        assert tile_start == GPU_BASE + 0x2000 and tile[0] == 125 and tile[-4:] == b'\x19\x03\x1b\x12'
        position, source = 1, None
        if tile[position] == 30:
            assert struct.unpack_from('<I', tile, position + 4)[0] >> 4 == width * 4
            source = struct.unpack_from('<I', tile, position + 9)[0]
            assert source == GPU_BASE + SURFACE
            position += 13
        assert tile[position] == 26 and tile[position + 1] == 29
        position += 1
        assert struct.unpack_from('<H', tile, position + 2)[0] == 27 << 4
        assert struct.unpack_from('<I', tile, position + 4)[0] >> 4 == width * 4
        destination = struct.unpack_from('<I', tile, position + 9)[0]
        size = width * height * 4
        if source is None:
            assert destination == GPU_BASE + SURFACE and not self.target_mapped()
            pixels = bytearray(color * (width * height))
        else:
            assert destination == TARGET_BASE
            window = self.tlb[(TARGET_BASE - GPU_BASE) >> 12:][:512]
            assert all(window[:size >> 12]) and not any(window[size >> 12:])
            pixels = bytearray(self.gpu_read(source, size))
            self.presented.append(self.translate(destination))
        if self.corrupt: pixels[20] ^= 1
        if self.stall:
            self.gpu_write(destination, pixels[:size // 2 & ~4095])
            self.busy = True
            return
        self.gpu_write(destination, pixels)
        self.render_jobs += 1
        self.rfc = (self.rfc + 1) & 255
