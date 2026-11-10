"""Exercise the Python API against the actual native storage implementation."""
from pathlib import Path
import os
import sys

sys.path.insert(0, os.environ.get('IISOCIETYCONTAINER_PYTHON_DIR',
    str(Path(__file__).resolve().parents[1] / 'src' / 'Storage')))
from mobile_storage import MobileStorage, MobileStorageError

tool, workspace = sys.argv[1], Path(sys.argv[2])
storage = MobileStorage(tool)
private = workspace / 'private'
shared = workspace / 'shared'
private.mkdir()
shared.mkdir()
source = storage.prepare(private)
destination = storage.prepare(shared)
assert source.root != destination.root and source.identifier != destination.identifier
source_file = source.root / 'Files' / 'canvas.iisc'
source_file.write_bytes(b'private canvas')
(destination.root / 'Files' / 'canvas.iisc').write_bytes(b'existing canvas')
result = storage.migrate(source, destination.root)
assert (result.copied, result.already_present, result.conflicts) == (1, 0, 1)
assert source_file.read_bytes() == b'private canvas'
assert (destination.root / 'Files' / 'canvas.iisc').read_bytes() == b'existing canvas'
result = storage.migrate(source, destination.root)
assert (result.copied, result.already_present) == (0, 1)
try:
    storage.prepare(workspace / 'missing')
except MobileStorageError:
    pass
else:
    raise AssertionError('invalid application directory was accepted')
