"""Sector-device callbacks for linked persistent-file tests, not SD hardware."""
import pathlib
import struct
import sys

from unicorn import UC_HOOK_CODE
from unicorn import arm64_const as regs

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
from storage_image import new_image
from tasks import SchedulerMachine

OPS = 0x7300000
CALLBACK = 0x7310000


class Disk:
    """Mixin: a modeled sector device the kernel reaches through code hooks."""
    ops, callback = OPS, CALLBACK

    def attach_disk(self, disk=None):
        self.disk = bytearray(new_image() if disk is None else disk)
        self.uc.mem_write(self.ops, struct.pack('<5Q', 0, len(self.disk) // 512,
                                              self.callback, self.callback + 4, self.callback + 8))
        for i in range(3):
            self.uc.hook_add(UC_HOOK_CODE, self.sector, user_data=i,
                             begin=self.callback + i * 4, end=self.callback + i * 4)

    def sector(self, uc, address, size, operation):
        if operation != 2:
            lba, data = uc.reg_read(regs.UC_ARM64_REG_X1), uc.reg_read(regs.UC_ARM64_REG_X2)
            assert lba < len(self.disk) // 512
            if operation == 0:
                uc.mem_write(data, bytes(self.disk[lba * 512:(lba + 1) * 512]))
            else:
                assert 2048 <= lba < len(self.disk) // 512
                self.disk[lba * 512:(lba + 1) * 512] = uc.mem_read(data, 512)
        uc.reg_write(regs.UC_ARM64_REG_X0, 0)
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    def mount_files(self):
        return self.call('files_mount', self.ops, 0)


class FileMachine(Disk, SchedulerMachine):
    def __init__(self, image, disk=None):
        super().__init__(image)
        self.attach_disk(disk)
