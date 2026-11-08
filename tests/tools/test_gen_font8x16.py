import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[2] / 'drivers/video/gen_font8x16.py'
spec = importlib.util.spec_from_file_location('gen_font8x16', SCRIPT)
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class FontDownloadTests(unittest.TestCase):
    def test_private_download_ignores_hostile_cache_and_cleans_up(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            victim = root / 'victim'
            victim.write_bytes(b'unchanged')
            cache = root / 'vantage-font'
            cache.mkdir()
            (cache / 'JetBrainsMono-Regular.ttf').symlink_to(victim)
            sources = []

            def choose(source):
                sources.append(source)
                self.assertEqual(source.read_bytes(), b'downloaded font')
                self.assertNotEqual(source.parent, cache)
                if os.name == 'posix':
                    self.assertEqual(source.parent.stat().st_mode & 0o777, 0o700)
                    self.assertEqual(source.parent.stat().st_uid, os.getuid())
                return {'placements': {}, 'baseline': 0}

            with mock.patch.object(tempfile, 'tempdir', str(root)), \
                    mock.patch('sys.argv', [str(SCRIPT)]), \
                    mock.patch.object(generator.urllib.request, 'urlopen',
                                      return_value=io.BytesIO(b'downloaded font')) as download, \
                    mock.patch.object(generator, 'choose_font', side_effect=choose), \
                    mock.patch.object(generator, 'cell_rows', return_value={' ': [0]}), \
                    mock.patch.object(generator, 'write_header') as write:
                generator.main()
                download.assert_called_once_with(generator.FONT_URL, timeout=60)
                write.assert_called_once()
            self.assertEqual(victim.read_bytes(), b'unchanged')
            self.assertTrue((cache / 'JetBrainsMono-Regular.ttf').is_symlink())
            self.assertFalse(sources[0].parent.exists())

    def test_failed_download_cleans_up(self):
        with tempfile.TemporaryDirectory() as root:
            with mock.patch.object(tempfile, 'tempdir', root), \
                    mock.patch('sys.argv', [str(SCRIPT)]), \
                    mock.patch.object(generator.urllib.request, 'urlopen',
                                      side_effect=OSError('download failed')):
                with self.assertRaisesRegex(OSError, 'download failed'):
                    generator.main()
            self.assertEqual(list(Path(root).iterdir()), [])

    def test_local_font_does_not_download_or_create_temporary_directory(self):
        with tempfile.TemporaryDirectory() as root:
            source = Path(root) / 'local.ttf'
            source.write_bytes(b'local font')
            with mock.patch('sys.argv', [str(SCRIPT), '--ttf', str(source)]), \
                    mock.patch.object(generator.urllib.request, 'urlopen') as download, \
                    mock.patch.object(tempfile, 'TemporaryDirectory') as temporary, \
                    mock.patch.object(generator, 'choose_font',
                                      return_value={'placements': {}, 'baseline': 0}) as choose, \
                    mock.patch.object(generator, 'cell_rows', return_value={' ': [0]}), \
                    mock.patch.object(generator, 'write_header'):
                generator.main()
                choose.assert_called_once_with(source)
                download.assert_not_called()
                temporary.assert_not_called()
            self.assertEqual(source.read_bytes(), b'local font')


if __name__ == '__main__':
    unittest.main()
