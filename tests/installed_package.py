"""Configure, build and execute a staged Qt-free consumer with native home paths."""
import os
from pathlib import Path
import re
import subprocess
import sys

build = Path(sys.argv[1]).resolve()
cmake = sys.argv[2]
source = Path(__file__).resolve().parent / 'native_consumer'
stage = build / 'tests/installed-package/prefix'
consumer = build / 'tests/installed-package/build'
cache = dict(re.findall(r'^([^/#\r\n][^:\r\n]*):[^=\r\n]+=(.*)$', (build / 'CMakeCache.txt').read_text(encoding='utf-8'), re.M))
env = os.environ.copy()

def run(command):
    completed = subprocess.run(list(map(str, command)), env=env, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=150)
    if completed.returncode:
        raise RuntimeError(f'{command}\n{completed.stdout}\n{completed.stderr}')

run([cmake, '--install', build, '--config', cache.get('CMAKE_BUILD_TYPE', 'Release'), '--prefix', stage])
config = [cmake, '-S', source, '-B', consumer, '-G', cache['CMAKE_GENERATOR'],
          '-DCMAKE_BUILD_TYPE=' + cache.get('CMAKE_BUILD_TYPE', 'Release'),
          '-DCMAKE_CXX_COMPILER=' + cache['CMAKE_CXX_COMPILER'],
          '-DCMAKE_PREFIX_PATH=' + stage.as_posix() + ';' + cache.get('CMAKE_PREFIX_PATH', ''),
          '-DiiSocietyContainer_DIR=' + (stage / 'lib/cmake/iiSocietyContainer').as_posix()]
for name in ['CMAKE_TOOLCHAIN_FILE', 'VCPKG_TARGET_TRIPLET', 'CMAKE_PROJECT_INCLUDE']:
    if cache.get(name):
        config.append('-D' + name + '=' + cache[name])
run(config)
run([cmake, '--build', consumer, '--config', cache.get('CMAKE_BUILD_TYPE', 'Release'), '--parallel', '2'])
env['PATH'] = str(stage / 'bin') + os.pathsep + env['PATH']
run([str(Path(cmake).with_name('ctest.exe' if os.name == 'nt' else 'ctest')), '--test-dir', consumer, '-C', cache.get('CMAKE_BUILD_TYPE', 'Release'), '--output-on-failure', '--no-tests=error'])
print('Staged FileTree/Maintenance consumers execute without importing Qt.')
