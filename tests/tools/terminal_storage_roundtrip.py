"""Exercise terminal file commands against a regular disk image across reboots."""
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
from storage_image import new_image

with tempfile.TemporaryDirectory(prefix='vantage-editor-') as directory:
    path = pathlib.Path(directory) / 'storage.img'
    original = new_image()
    path.write_bytes(original)
    for action in ('write', 'read', 'deleted'):
        subprocess.run([sys.argv[1], action, str(path)], check=True)
    assert path.read_bytes()[:2048 * 512] == original[:2048 * 512]
