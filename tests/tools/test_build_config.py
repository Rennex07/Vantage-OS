import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'tools' / 'build_config.py'


class BuildConfigTests(unittest.TestCase):
    def test_unchanged_configuration_keeps_timestamp_and_changed_flags_replace_it(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'nested' / 'build-config'
            command = [sys.executable, '-B', str(SCRIPT), '--output', str(output),
                       '--cc=gcc', '--ld=ld', '--objcopy=objcopy', '--firmware=']
            subprocess.run(command + ['--cflags=-DVANTAGE_STORAGE_VALIDATION=0'], check=True)
            os.utime(output, ns=(1_000_000_000, 1_000_000_000))
            stamp = output.stat().st_mtime_ns
            subprocess.run(command + ['--cflags=-DVANTAGE_STORAGE_VALIDATION=0'], check=True)
            self.assertEqual(output.stat().st_mtime_ns, stamp)
            subprocess.run(command + ['--cflags=-DVANTAGE_STORAGE_VALIDATION=1'], check=True)
            self.assertNotEqual(output.stat().st_mtime_ns, stamp)
            self.assertEqual(json.loads(output.read_text())['cflags'], '-DVANTAGE_STORAGE_VALIDATION=1')


if __name__ == '__main__':
    unittest.main()
