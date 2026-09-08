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
    container = temporary / 'Society'
    container.mkdir()
    subprocess.run([tool, 'create', str(container)], check=True, capture_output=True)
    (container / 'Models/private.bin').write_bytes(b'private')
    env = os.environ.copy()
    env['SOCIETY_MOUNT_STATE_DIRECTORY'] = str(temporary / 'state')
    env['SOCIETY_MOUNT_AUTOSTART'] = '0'
    def call(action, argument='', success=True):
        result = subprocess.run([service, action, argument], env=env, capture_output=True, text=True, timeout=25)
        data = json.loads(result.stdout)
        assert (result.returncode == 0) == success, (result.returncode, data, result.stderr)
        return data
    def start():
        process = subprocess.Popen([service, 'serve'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(100):
            if (temporary / 'state/service.lock').is_file():
                time.sleep(0.1)
                return process
            assert process.poll() is None
            time.sleep(0.05)
        raise AssertionError('The test service did not start')
    process = start()
    try:
        registered = call('register', str(container))
        identifier = registered['identifier']
        assert registered['systemPath'] == str(container / 'Files')
        assert registered['enabled']
        assert call('register', str(container))['identifier'] == identifier
        assert call('path', identifier)['systemPath'] == str(container / 'Files')
        assert len(call('list')['drives']) == 1
        call('register', str(temporary / 'missing'), success=False)
        process.terminate()
        process.wait(timeout=10)
        process = start()
        assert call('refresh', identifier)['enabled']
        # A new manifest at the same path must not inherit the old mount identity.
        (container / '.society-drive.json').unlink()
        subprocess.run([tool, 'create', str(container)], check=True, capture_output=True)
        call('refresh', identifier, success=False)
        assert call('unregister', identifier)['enabled'] is False
        assert call('list')['drives'] == []
        assert (container / 'Models/private.bin').read_bytes() == b'private'
    finally:
        process.terminate()
        process.wait(timeout=10)
print('Registration, restart, identity replacement and unregister passed (stub mount).')
