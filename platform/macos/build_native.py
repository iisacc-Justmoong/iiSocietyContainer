#!/usr/bin/env python3
"""Build the native adapter with Command Line Tools; Xcode is not required."""
import argparse
import pathlib
import plistlib
import platform
import subprocess


def run(*command):
    subprocess.run(command, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--catalog-tool", required=True)
    parser.add_argument("--sign", default="-")
    parser.add_argument("--version", required=True)
    parser.add_argument("--tests", action="store_true")
    args = parser.parse_args()
    source = pathlib.Path(__file__).resolve().parent
    shared = source.parent / "apple"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    catalog = subprocess.check_output([args.catalog_tool, "catalog"])
    (output / "Sections.json").write_bytes(catalog)
    app = output / "Society Container.app"
    provider = app / "Contents/PlugIns/SocietyContainerProvider.appex"
    identifier = "com.iisacc.society.container.drive"
    for bundle, executable, bundle_id, package_type in [
        (app, "SocietyContainerDrive", identifier, "APPL"),
        (provider, "SocietyContainerProvider", identifier + ".provider", "XPC!"),
    ]:
        (bundle / "Contents/MacOS").mkdir(parents=True, exist_ok=True)
        (bundle / "Contents/Resources").mkdir(parents=True, exist_ok=True)
        (bundle / "Contents/Resources/Sections.json").write_bytes(catalog)
        info = dict(CFBundleIdentifier=bundle_id, CFBundleExecutable=executable,
                    CFBundleName="Society Container", CFBundleDisplayName="Society Container",
                    CFBundlePackageType=package_type, CFBundleVersion=args.version,
                    CFBundleShortVersionString=args.version, LSMinimumSystemVersion="15.0")
        if bundle == app:
            info["LSUIElement"] = True
        else:
            info["NSExtension"] = {
                "NSExtensionPointIdentifier": "com.apple.fileprovider-nonui",
                "NSExtensionPrincipalClass": "SocietyContainerFileProvider",
                "NSExtensionFileProviderSupportsEnumeration": True,
            }
            info["CFBundleIcons"] = {"CFBundlePrimaryIcon": {"CFBundleSymbolName": "externaldrive"}}
        (bundle / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
    compiler = ["xcrun", "swiftc", "-swift-version", "5", "-O",
                "-target", platform.machine() + "-apple-macosx15.0",
                "-module-cache-path", str(output / "module-cache"),
                "-framework", "Foundation", "-framework", "CryptoKit"]
    run(*compiler, "-module-name", "SocietyContainerProvider", "-parse-as-library", "-emit-executable",
        "-Xlinker", "-e", "-Xlinker", "_NSExtensionMain", "-framework", "FileProvider",
        "-framework", "UniformTypeIdentifiers", "-framework", "CoreServices",
        str(shared / "LocalDriveStore.swift"), str(shared / "FilesDriveStore.swift"),
        str(shared / "FileProviderExtension.swift"),
        "-o", str(provider / "Contents/MacOS/SocietyContainerProvider"))
    run(*compiler, "-framework", "FileProvider", str(shared / "LocalDriveStore.swift"),
        str(source / "DriveHost.swift"), "-o", str(app / "Contents/MacOS/SocietyContainerDrive"))
    entitlements = output / "provider.entitlements"
    entitlements.write_bytes(plistlib.dumps({
        "com.apple.security.app-sandbox": True,
        "com.apple.security.files.user-selected.read-write": True,
        "com.apple.security.files.bookmarks.app-scope": True,
    }))
    run("codesign", "--force", "--timestamp=none", "--sign", args.sign,
        "--entitlements", str(entitlements), str(provider))
    run("codesign", "--force", "--timestamp=none", "--sign", args.sign, str(app))
    if args.tests:
        run(*compiler, str(shared / "LocalDriveStore.swift"), str(shared / "FilesDriveStore.swift"),
            str(shared / "SharedDriveLocation.swift"), str(shared / "SharedDriveLocationTests.swift"),
            "-o", str(output / "SharedDriveLocationTests"))
        run(*compiler, str(shared / "LocalDriveStore.swift"), str(source / "LocalDriveStoreTests.swift"),
            "-o", str(output / "LocalDriveStoreTests"))
        run(*compiler, str(shared / "LocalDriveStore.swift"), str(shared / "FilesDriveStore.swift"),
            str(shared / "FileProviderExtension.swift"), str(source / "FilesDriveStoreTests.swift"),
            "-framework", "FileProvider", "-framework", "UniformTypeIdentifiers", "-framework", "CoreServices",
            "-o", str(output / "FilesDriveStoreTests"))


if __name__ == "__main__":
    main()
