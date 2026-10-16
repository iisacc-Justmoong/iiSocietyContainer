"""Service/registration lifecycle against a stub mount; never an OS mount test."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

tool, service, build = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='mount-service-', dir=build) as temporary:
    temporary = Path(temporary)
    container = temporary / 'Society.societycontainer'
    container.mkdir()
    initial = json.loads(subprocess.check_output([tool, 'create', str(container)], text=True))
    (container / 'Models/private.bin').write_bytes(b'private')
    env = os.environ.copy()
    selected = temporary / 'selected-storage.json'
    selected.write_text(json.dumps({'schemaVersion': 1, 'containerId': initial['identifier'], 'path': str(container)}))
    env['SOCIETY_STORAGE_SETTINGS_PATH'] = str(selected)
    # A previous external state override must no longer create managed files.
    env['SOCIETY_MOUNT_STATE_DIRECTORY'] = str(temporary / 'outside-state')
    env['SOCIETY_MOUNT_AUTOSTART'] = '0'
    def call(action, argument='', success=True):
        result = subprocess.run([service, action, argument], env=env, capture_output=True, text=True, timeout=25)
        data = json.loads(result.stdout)
        assert (result.returncode == 0) == success, (result.returncode, data, result.stderr)
        return data
    def start():
        process = subprocess.Popen([service, 'serve', str(container)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(100):
            if (container / '.society-runtime/mount-service/service.lock').is_file():
                time.sleep(0.1)
                return process
            assert process.poll() is None
            time.sleep(0.05)
        raise AssertionError('The test service did not start')
    process = start()
    try:
        registered = call('register', str(container))
        identifier = registered['identifier']
        assert Path(registered['systemPath']) == container / 'Files'
        assert registered['enabled']
        assert (container / '.society-runtime/mount-service/drives.json').is_file()
        assert not (temporary / 'outside-state').exists(), 'Container management must ignore external state-directory overrides'
        assert registered['isMounted'] is False and registered['filesystemType'] == '', 'A stub must not claim an OS mount'
        assert call('mount', str(container))['systemPath'] == registered['systemPath']
        assert call('register', str(container))['identifier'] == identifier
        assert Path(call('path', identifier)['systemPath']) == container / 'Files'
        assert len(call('list')['drives']) == 1
        call('register', str(temporary / 'missing'), success=False)
        process.terminate()
        process.wait(timeout=10)
        process = start()
        assert call('refresh', identifier)['enabled']
        assert call('unmount', identifier)['isMounted'] is False
        assert call('mount', str(container))['identifier'] == identifier
        # A new manifest at the same path must not inherit the old mount identity.
        (container / '.society-drive.json').unlink()
        replacement = json.loads(subprocess.check_output([tool, 'create', str(container)], text=True))
        call('refresh', identifier, success=False)
        selected.write_text(json.dumps({'schemaVersion': 1, 'containerId': replacement['identifier'], 'path': str(container)}))
        assert call('unregister', identifier)['enabled'] is False
        for _ in range(20):
            assert call('list')['drives'] == []
        assert (container / 'Models/private.bin').read_bytes() == b'private'
    finally:
        process.terminate()
        process.wait(timeout=10)
print('Registration, restart, identity replacement and unregister passed (stub mount).')
