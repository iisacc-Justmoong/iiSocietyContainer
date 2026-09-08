#!/usr/bin/env python3
"""Install a test-signed Society APK and exercise its registered Android provider.

Use a disposable emulator: this installs com.iisacc.society and creates test files.
All host artifacts, including the test signing key, stay in the supplied build/ path.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apk', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--java', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--serial', required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[2]
    build = args.build.resolve()
    if 'build' not in build.parts:
        raise SystemExit('Test artifacts must be under build/')
    build.mkdir(parents=True, exist_ok=True)
    sdk, java = args.sdk.resolve(), args.java.resolve()
    android = sdk / 'platforms/android-36/android.jar'
    tools = sdk / 'build-tools/36.0.0'
    env = os.environ.copy()
    env['JAVA_HOME'] = str(java)
    env['ANDROID_USER_HOME'] = str(build / 'user')
    adb = [str(sdk / 'platform-tools/adb'), '-s', args.serial]

    def run(command):
        result = subprocess.run([str(value) for value in command], env=env, capture_output=True, text=True, timeout=180)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout

    # Do not replace an existing app with a different signing identity. adb install
    # rejects that case; the runner deliberately does not uninstall user packages.
    key = build / 'test.jks'
    if not key.exists():
        run([java / 'bin/keytool', '-genkeypair', '-keystore', key, '-storepass', 'society-test', '-keypass', 'society-test',
             '-alias', 'society', '-keyalg', 'RSA', '-validity', '3650', '-dname', 'CN=Society Emulator Tests'])
    signing = [tools / 'apksigner', 'sign', '--ks', key, '--ks-pass', 'pass:society-test', '--key-pass', 'pass:society-test']
    apk = build / 'Society-test.apk'
    shutil.copyfile(args.apk, apk)
    run(signing + [apk])
    run(adb + ['install', '-r', apk])
    classes, dex = build / 'classes', build / 'dex'
    classes.mkdir(exist_ok=True); dex.mkdir(exist_ok=True)
    provider = build / 'provider-classes'
    provider.mkdir(exist_ok=True)
    run([java / 'bin/javac', '-source', '8', '-target', '8', '-classpath', android, '-d', provider]
        + sorted((source / 'platform/android/src').rglob('*.java')))
    run([java / 'bin/javac', '-source', '8', '-target', '8', '-classpath', str(android) + os.pathsep + str(provider), '-d', classes,
         source / 'tests/android/ProviderInstrumentation.java'])
    run([tools / 'd8', '--lib', android, '--min-api', '28', '--output', dex] + sorted(classes.rglob('*.class')))
    manifest = build / 'AndroidManifest.xml'
    manifest.write_text('''<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.iisacc.society.storage.tests">
<uses-sdk android:minSdkVersion="28" android:targetSdkVersion="35" />
<application android:label="Society Storage Tests" android:hasCode="true" />
<instrumentation android:name="com.iisacc.society.storage.tests.ProviderInstrumentation" android:targetPackage="com.iisacc.society" />
</manifest>''')
    unsigned, test = build / 'tests-unsigned.apk', build / 'tests.apk'
    run([tools / 'aapt2', 'link', '-I', android, '--manifest', manifest, '-o', unsigned])
    with zipfile.ZipFile(unsigned, 'a') as archive:
        for path in dex.glob('*.dex'):
            archive.write(path, path.name)
    run([tools / 'zipalign', '-f', '4', unsigned, test])
    run(signing + [test])
    run(adb + ['install', '-r', test])
    for phase in ('operations', 'restart'):
        run(adb + ['shell', 'am', 'force-stop', 'com.iisacc.society'])
        output = run(adb + ['shell', 'am', 'instrument', '-w', '-r', '-e', 'phase', phase,
                            'com.iisacc.society.storage.tests/com.iisacc.society.storage.tests.ProviderInstrumentation'])
        (build / f'{phase}.log').write_text(output)
        print(output)
        if 'checks passed' not in output or 'INSTRUMENTATION_CODE: -1' not in output:
            raise SystemExit('Android provider device checks failed')


if __name__ == '__main__':
    main()
