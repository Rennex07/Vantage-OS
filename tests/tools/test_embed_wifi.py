import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class EmbedTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name, data in [('firmware', b'\x01\x02\x03\x04' * 8),
                           ('nvram', b'boardtype=0x1\n'), ('clm', b'synthetic-test-data')]:
            (self.root / name).write_bytes(data)
        self.output = self.root / 'assets.c'

    def run_tool(self, *extra):
        command = [sys.executable, str(ROOT / 'tools/embed_wifi.py'),
                   *[arg for name in ('firmware', 'nvram', 'clm') for arg in
                     ('--' + name, str(self.root / name))],
                   '--country', 'GB', '--regrev', '0', '--output', str(self.output), *extra]
        return subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_reproducible_output_and_hash_manifest(self):
        self.assertEqual(self.run_tool().returncode, 0)
        before = self.output.read_bytes()
        self.assertIn(b'#include <vantage/drivers/wifi.h>', before)
        self.assertEqual(self.run_tool().returncode, 0)
        self.assertEqual(before, self.output.read_bytes())
        data = json.loads(self.output.with_suffix('.json').read_text())
        self.assertEqual(data['firmware']['bytes'], 32)
        self.assertEqual(len(data['firmware']['sha256']), 64)
        self.assertEqual(data['country'], 'GB')

    def test_explicit_regulatory_values(self):
        for country in ('', '00', 'gb', 'GBR', 'G"'):
            self.assertNotEqual(self.run_tool('--country', country).returncode, 0)
        self.assertNotEqual(self.run_tool('--regrev', '-2').returncode, 0)

    def test_no_overwrite_of_unrelated_files_or_inputs(self):
        self.output.write_text('user-owned content', encoding='utf-8')
        self.assertNotEqual(self.run_tool().returncode, 0)
        self.assertEqual(self.output.read_text(), 'user-owned content')
        self.assertNotEqual(self.run_tool('--output', str(self.root / 'firmware')).returncode, 0)

    def test_invalid_images_and_binary_nvram_rejected(self):
        (self.root / 'firmware').write_bytes(b'\0' * 32)
        self.assertNotEqual(self.run_tool().returncode, 0)
        (self.root / 'firmware').write_bytes(b'\x01\x02\x03\x04')
        (self.root / 'nvram').write_bytes(b'a=1\0')
        self.assertNotEqual(self.run_tool().returncode, 0)
        self.assertFalse(self.output.exists())

    def test_unrelated_manifest_is_preserved(self):
        manifest = self.output.with_suffix('.json')
        manifest.write_text('{"user": true}', encoding='utf-8')
        self.assertNotEqual(self.run_tool().returncode, 0)
        self.assertEqual(manifest.read_text(), '{"user": true}')
        self.assertFalse(self.output.exists())


if __name__ == '__main__': unittest.main()
