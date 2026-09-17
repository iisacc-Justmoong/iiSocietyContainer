#!/usr/bin/env python3
"""Check iOS packaging contracts on the host; this is not an iOS runtime test."""
import argparse
import json
import pathlib
import plistlib
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--platform', type=pathlib.Path, required=True)
    parser.add_argument('--catalog-tool', required=True)
    parser.add_argument('--build', type=pathlib.Path, required=True)
    args = parser.parse_args()
    output = args.build.resolve() / 'ios-package-contract'
    output.mkdir(parents=True, exist_ok=True)
    activity = args.platform / 'ios/live-activity'
    state_test = output / 'TaskActivityStateTests'
    subprocess.run(['xcrun', 'swiftc', '-parse-as-library', str(activity / 'TaskActivityState.swift'),
                    str(activity / 'TaskActivityStateTests.swift'), '-o', str(state_test)], check=True)
    subprocess.run([str(state_test)], check=True)
    values = {
        'SOCIETY_IOS_APP_IDENTIFIER': 'com.iisacc.society',
        'SOCIETY_IOS_PROVIDER_IDENTIFIER': 'com.iisacc.society.fileprovider',
        'SOCIETY_IOS_GROUP': 'group.com.iisacc.society',
        'SOCIETY_IOS_DISPLAY_NAME': 'Society',
        'SOCIETY_IOS_VERSION': '0.1.0', 'SOCIETY_IOS_MINIMUM': '16.0',
    }
    script = [f'set({key} "{value}")' for key, value in values.items()]
    names = ('App.Info.plist', 'Provider.Info.plist', 'App.entitlements', 'Provider.entitlements')
    for name in names:
        script.append(f'configure_file("{args.platform / "ios" / (name + ".in")}" "{output / name}" @ONLY)')
    configure = output / 'configure.cmake'
    configure.write_text('\n'.join(script) + '\n')
    subprocess.run(['cmake', '-P', str(configure)], check=True)
    app, provider, app_rights, provider_rights = [plistlib.loads((output / name).read_bytes()) for name in names]
    assert app['CFBundleIdentifier'] + '.fileprovider' == provider['CFBundleIdentifier']
    assert app['CFBundleVersion'] == provider['CFBundleVersion']
    assert app['MinimumOSVersion'] == provider['MinimumOSVersion'] == '16.0'
    assert app['UIFileSharingEnabled'] is False, 'Do not publish the complete app Documents directory'
    model_type = app['UTImportedTypeDeclarations'][0]
    assert model_type['UTTypeConformsTo'] == ['public.data']
    assert model_type['UTTypeTagSpecification']['public.filename-extension'] == ['safetensor', 'safetensors']
    assert 'UIBackgroundModes' not in app, 'Do not declare unrelated modes to keep a daemon alive'
    group = app['SocietyAppGroup']
    assert provider['SocietyAppGroup'] == group
    for rights in (app_rights, provider_rights):
        assert rights['com.apple.security.application-groups'] == [group]
        assert 'com.apple.security.files.bookmarks.app-scope' not in rights
    extension = provider['NSExtension']
    assert app['CFBundleDisplayName'] == 'Society'
    assert provider['CFBundleName'] == provider['CFBundleDisplayName'] == 'Society'
    assert extension['NSExtensionPointIdentifier'] == 'com.apple.fileprovider-nonui'
    assert extension['NSExtensionPrincipalClass'] == 'SocietyContainerFileProvider'
    assert extension['NSExtensionFileProviderSupportsEnumeration'] is True
    assert extension['NSExtensionFileProviderDocumentGroup'] == group
    # Exercise the actual client helper on the host with a synthetic iOS target.
    client = output / 'client'
    client.mkdir(exist_ok=True)
    (client / 'empty.cpp').write_text('int main() { return 0; }\n')
    (client / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.24)
project(Client VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_DEPLOYMENT_TARGET 16.0)
include("{args.platform / 'ios/iiSocietyContainerIOS.cmake'}")
add_executable(Dreamscapes empty.cpp)
set_target_properties(Dreamscapes PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER com.iisacc.dreamscapes)
iiSocietyContainer_configure_ios_client(Dreamscapes APP_GROUP {group} DISPLAY_NAME Dreamscapes)
''')
    subprocess.run(['cmake', '-S', str(client), '-B', str(client / 'build')], check=True, stdout=subprocess.DEVNULL)
    generated = client / 'build/ios-society-client/Dreamscapes'
    client_info = plistlib.loads((generated / 'App.Info.plist').read_bytes())
    client_rights = plistlib.loads((generated / 'App.entitlements').read_bytes())
    assert client_info['CFBundleDisplayName'] == 'Dreamscapes'
    assert client_info['CFBundleIdentifier'] == 'com.iisacc.dreamscapes'
    assert client_info['SocietyAppGroup'] == group
    assert client_info['UIFileSharingEnabled'] is False
    assert client_rights['com.apple.security.application-groups'] == [group]
    assert not (generated / 'Provider.Info.plist').exists()
    assert json.loads((args.platform / 'apple/Sections.json').read_bytes()) == json.loads(
        subprocess.check_output([args.catalog_tool, 'catalog']))
    # Parse the actual iOS conditional branches without claiming SDK typechecking.
    sources = [*sorted((args.platform / 'apple').glob('*.swift')), *sorted((args.platform / 'ios').glob('*.swift'))]
    sources = [str(path) for path in sources if not path.name.endswith('Tests.swift')]
    subprocess.run(['xcrun', 'swiftc', '-frontend', '-parse', '-target', 'arm64-apple-ios16.0', *sources], check=True)
    sdk = subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'],
                         capture_output=True, text=True)
    if sdk.returncode == 0:
        native = output / 'native'
        native.mkdir(exist_ok=True)
        (native / 'main.cpp').write_text('int main() { return 0; }\n')
        (native / 'probe.cpp').write_text('int packaging_cpp_probe() { return 0; }\n')
        (native / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.24)
project(NativePackage VERSION 0.1.0 LANGUAGES CXX Swift)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
include("{args.platform / 'ios/iiSocietyContainerIOS.cmake'}")
add_executable(Society MACOSX_BUNDLE main.cpp)
set_target_properties(Society PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER com.iisacc.society)
iiSocietyContainer_add_ios_file_provider(Society APP_GROUP {group})
iiSocietyContainer_add_ios_live_activity(Society)
# Exercise mixed-language compilation, including the C++ objects Qt autogen
# previously injected into native Swift targets.
target_sources(SocietyIosDriveBridge PRIVATE probe.cpp)
target_sources(SocietyFileProvider PRIVATE probe.cpp)
''')
        subprocess.run(['cmake', '-S', str(native), '-B', str(native / 'build'), '-G', 'Xcode',
                        '-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_OSX_SYSROOT=iphoneos',
                        '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=16.0',
                        '-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO'], check=True)
        subprocess.run(['cmake', '--build', str(native / 'build'), '--config', 'Debug',
                        '--target', 'SocietyIosDriveBridge', 'SocietyFileProvider', 'SocietyActivityBridge', 'SocietyLiveActivity',
                        '--parallel', '2'], check=True)
        widget = native / 'build/Debug-iphoneos/SocietyLiveActivity.appex'
        widget_info = plistlib.loads((widget / 'Info.plist').read_bytes())
        assert widget_info['CFBundleIdentifier'] == 'com.iisacc.society.liveactivity'
        assert widget_info['NSExtension']['NSExtensionPointIdentifier'] == 'com.apple.widgetkit-extension'
        activity_info = plistlib.loads((native / 'build/ios-live-activity/Society/App.Info.plist.in').read_bytes())
        assert activity_info['NSSupportsLiveActivities'] is True
        print('Native iOS bridge and extension compilation/link passed (no device runtime test)')
    else:
        print('Native iOS build skipped: the iPhoneOS SDK is unavailable')
    print('iOS app/extension identifiers, entitlements, catalog and Swift syntax passed')


if __name__ == '__main__':
    main()
