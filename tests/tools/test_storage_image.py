from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from storage_image import BANK, MIN_SECTORS, SECTOR, inspect_bank, new_image, partition


class ImageTests(unittest.TestCase):
    def test_format_and_inspect_regular_image_preserves_surrounding_sectors(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'storage.img'
            command = [sys.executable, '-B', str(ROOT / 'tools' / 'storage_image.py')]
            subprocess.run(command + ['create', '--image', str(path)], check=True, capture_output=True)
            original = path.read_bytes()
            self.assertEqual(partition(original[:512], len(original) // 512, 0), (2048, MIN_SECTORS))
            for bank in range(2):
                start = (2048 + bank * BANK) * SECTOR
                self.assertTrue(inspect_bank(original[start:start + BANK * SECTOR], bank, 2048, MIN_SECTORS)['valid'])
            self.assertNotEqual(subprocess.run(command + ['create', '--image', str(path)], capture_output=True).returncode, 0)
            self.assertNotEqual(subprocess.run(command + ['format', '--image', str(path)], capture_output=True).returncode, 0)
            self.assertEqual(path.read_bytes(), original)
            subprocess.run(command + ['format', '--image', str(path), '--confirm-format'], check=True, capture_output=True)
            self.assertEqual(path.read_bytes(), original)
            subprocess.run(command + ['inspect', '--image', str(path)], check=True, capture_output=True)
            corrupt = bytearray(original)
            corrupt[2048 * SECTOR] ^= 1
            corrupt[(2048 + BANK) * SECTOR] ^= 1
            path.write_bytes(corrupt)
            self.assertNotEqual(subprocess.run(command + ['inspect', '--image', str(path)], capture_output=True).returncode, 0)
            self.assertEqual(path.read_bytes(), corrupt)

    def test_partition_rejects_overlap_gpt_bounds_and_wrong_type(self):
        for kind in ('overlap', 'gpt', 'bounds', 'wrong-type', 'zero-start'):
            image = new_image()
            if kind == 'overlap': image[462:478] = image[446:462]
            if kind == 'gpt': image[450] = 0xee
            if kind == 'bounds': struct.pack_into('<I', image, 458, 0xffffffff)
            if kind == 'wrong-type': image[450] = 0x0c
            if kind == 'zero-start': struct.pack_into('<I', image, 454, 0)
            with self.assertRaises(ValueError): partition(image[:512], len(image) // 512, 0)


if __name__ == '__main__':
    unittest.main()
