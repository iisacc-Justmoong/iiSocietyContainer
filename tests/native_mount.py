#!/usr/bin/env python3
"""Real OS-mount integration test. Requires Dokan 2 or a usable Linux /dev/fuse.

Arguments: drive-tool mount-executable build-directory. Uses a new disposable drive,
disables login registration, and leaves every existing registration untouched.
"""
import json
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

tool, mount, build = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='native-files-', dir=build) as temporary:
    temporary = Path(temporary)
    source = temporary / 'Society.societycontainer'
    source.mkdir()
    info = json.loads(subprocess.check_output([tool, 'create', str(source)], text=True))
    identifier = info['identifier']
    manifest_hash = hashlib.sha256((source / '.society-drive.json').read_bytes()).hexdigest()
    (source / 'Models' / 'private.bin').write_bytes(b'private')
    (source / 'Files' / 'from-app.txt').write_text('from Society')
    env = os.environ.copy()
    selected = temporary / 'selected-storage.json'
    selected.write_text(json.dumps({'schemaVersion': 1, 'containerId': identifier, 'path': str(source)}))
    env.update(SOCIETY_STORAGE_SETTINGS_PATH=str(selected), SOCIETY_MOUNT_AUTOSTART='0',
               XDG_DATA_HOME=str(temporary / 'data'), XDG_CONFIG_HOME=str(temporary / 'config'))

    def call(action, argument='', success=True):
        process = subprocess.run([mount, action, argument], capture_output=True, text=True, env=env, timeout=35)
        value = json.loads(process.stdout)
        assert (process.returncode == 0) == success, (process.returncode, value, process.stderr)
        return value

    def start():
        server = subprocess.Popen([mount, 'serve', str(source)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(100):
            if server.poll() is not None:
                raise AssertionError(server.stderr.read().decode())
            if (source / '.society-runtime/mount-service/service.lock').exists():
                time.sleep(0.15)
                return server
            time.sleep(0.05)
        raise AssertionError('Mount service did not start')

    server = start()
    registered = False
    try:
        result = call('mount', str(source))
        drive = Path(result['systemPath'])
        registered = True
        assert result['isMounted'] and result['filesystemType'] == ('Society' if os.name == 'nt' else 'fuse.society')
        if os.name == 'nt':
            import ctypes
            from ctypes import wintypes
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.GetVolumeInformationW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD,
                ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD),
                wintypes.LPWSTR, wintypes.DWORD]
            kernel.GetVolumeInformationW.restype = wintypes.BOOL
            kernel.GetDriveTypeW.argtypes = [wintypes.LPCWSTR]
            kernel.GetDriveTypeW.restype = wintypes.UINT
            label, filesystem = ctypes.create_unicode_buffer(256), ctypes.create_unicode_buffer(256)
            serial, component, flags = wintypes.DWORD(), wintypes.DWORD(), wintypes.DWORD()
            assert kernel.GetVolumeInformationW(str(drive), label, 256, ctypes.byref(serial), ctypes.byref(component), ctypes.byref(flags), filesystem, 256)
            assert label.value == 'Society' and filesystem.value == 'Society'
            assert kernel.GetDriveTypeW(str(drive)) == 2, 'Explorer must recognize a removable drive'
        assert drive != source / 'Files', 'Test must use a real OS projection'
        if os.name != 'nt':
            assert drive == source / '.society-runtime/mounts' / identifier / 'Society', 'Linux drive backing directories belong to their container'
        assert (drive / 'from-app.txt').read_text() == 'from Society'
        assert {path.name for path in drive.iterdir()} == {'from-app.txt'}
        for name in ('Documents', 'Audios', '3D objects'):
            directory = drive / name
            directory.mkdir()
            child = directory / 'user.txt'
            child.write_bytes(b'user data')
            moved = drive / (name + '-renamed')
            directory.rename(moved)
            assert (moved / 'user.txt').read_bytes() == b'user data'
            (moved / 'user.txt').unlink()
            moved.rmdir()
            assert not directory.exists()
        for name in ('Models', 'Files', 'Deleted', '.society-drive.json'):
            assert not (drive / name).exists()
        folder = drive / '한글 folder'
        folder.mkdir()
        file = folder / 'document.bin'
        file.write_bytes(b'abcdefgh')
        assert (source / 'Files/한글 folder/document.bin').read_bytes() == b'abcdefgh'
        with file.open('r+b') as stream:
            stream.seek(2); stream.write(b'XY'); stream.truncate(5)
            stream.flush(); os.fsync(stream.fileno())
        assert file.read_bytes() == b'abXYe'
        moved = drive / 'moved.bin'
        file.rename(moved)
        folder.rmdir()
        assert moved.read_bytes() == b'abXYe'
        moved.unlink()
        (source / 'Files/new-from-app.txt').write_text('visible')
        assert (drive / 'new-from-app.txt').read_text() == 'visible'
        assert 'new-from-app.txt' in {path.name for path in drive.iterdir()}
        if os.name != 'nt':
            (source / 'Files/private-link').symlink_to(source / 'Models', target_is_directory=True)
            assert not (drive / 'private-link').exists()
            assert 'private-link' not in {path.name for path in drive.iterdir()}
            (source / 'Files/private-link').unlink()
        assert (source / 'Models/private.bin').read_bytes() == b'private'
        assert call('mount', str(source))['systemPath'] == str(drive)
        assert hashlib.sha256((source / '.society-drive.json').read_bytes()).hexdigest() == manifest_hash
        assert call('unmount', identifier)['isMounted'] is False
        registered = False
        drive = Path(call('mount', str(source))['systemPath'])
        registered = True
        server.terminate(); server.wait(timeout=15)
        server = start()
        drive = Path(call('refresh', identifier)['systemPath'])
        assert (drive / 'from-app.txt').read_text() == 'from Society'
        if os.name != 'nt':
            server.kill(); server.wait(timeout=15)
            server = start()
            drive = Path(call('refresh', identifier)['systemPath'])
            assert (drive / 'from-app.txt').read_text() == 'from Society'
        assert call('unregister', identifier)['enabled'] is False
        registered = False
        assert call('list')['drives'] == []
    finally:
        if registered:
            try: call('unregister', identifier)
            except Exception: pass
        server.terminate()
        try: server.wait(timeout=15)
        except subprocess.TimeoutExpired: server.kill(); server.wait()
print('Real OS drive: Files projection, CRUD, backing-store updates, boundaries and restart passed.')
