#!/usr/bin/env python3
"""Build the native adapter with Command Line Tools; Xcode is not required."""
import argparse
import pathlib
import plistlib
import platform
import shutil
import subprocess
import os

FILE_PROVIDER_SWIFT = pathlib.Path(os.environ.get("IIFILEPROVIDER_SWIFT_SOURCE", pathlib.Path(__file__).resolve().parents[3] / "iiFileProvider/swift/IisaccFileProvider.swift"))


def run(*command):
    subprocess.run(command, check=True)


def build_filesystem(source, output, app, team):
    """Use Xcode's normal provisioning for the restricted FSKit entitlement."""
    if not team:
        return
    filesystem = source / "FSKit"
    project = output / "SocietyDriveFS.xcodeproj"
    project.mkdir(exist_ok=True)
    files = [FILE_PROVIDER_SWIFT, *sorted(filesystem.glob("*.swift"))]
    objects = []
    refs, builds = [], []
    for index, path in enumerate(files, 20):
        ref, build = f"{index:024X}", f"{index + 100:024X}"
        refs.append(ref); builds.append(build)
        objects += [f'{ref} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.swift; path = "{path}"; sourceTree = "<absolute>"; }};',
                    f'{build} = {{isa = PBXBuildFile; fileRef = {ref}; }};']
    identifier = lambda number: f"{number:024X}"
    ids = [identifier(number) for number in range(1, 12)]
    root, group, product, target, phase, configs, config, project_configs, project_config, products, frameworks = ids
    objects += [
        f'{root} = {{isa = PBXProject; mainGroup = {group}; productRefGroup = {products}; buildConfigurationList = {project_configs}; targets = ({target},); compatibilityVersion = "Xcode 14.0"; }};',
        f'{group} = {{isa = PBXGroup; children = ({",".join(refs)}, {products},); sourceTree = "<group>"; }};',
        f'{products} = {{isa = PBXGroup; children = ({product},); name = Products; sourceTree = "<group>"; }};',
        f'{product} = {{isa = PBXFileReference; explicitFileType = "wrapper.app-extension"; path = SocietyDriveFS.appex; sourceTree = BUILT_PRODUCTS_DIR; }};',
        f'{target} = {{isa = PBXNativeTarget; name = SocietyDriveFS; productName = SocietyDriveFS; productReference = {product}; productType = "com.apple.product-type.app-extension"; buildConfigurationList = {configs}; buildPhases = ({phase},{frameworks},); buildRules = (); dependencies = (); }};',
        f'{phase} = {{isa = PBXSourcesBuildPhase; files = ({",".join(builds)},); buildActionMask = 2147483647; runOnlyForDeploymentPostprocessing = 0; }};',
        f'{frameworks} = {{isa = PBXFrameworksBuildPhase; files = (); buildActionMask = 2147483647; runOnlyForDeploymentPostprocessing = 0; }};',
        f'{configs} = {{isa = XCConfigurationList; buildConfigurations = ({config},); defaultConfigurationName = Release; }};',
        f'{project_configs} = {{isa = XCConfigurationList; buildConfigurations = ({project_config},); defaultConfigurationName = Release; }};',
        f'{project_config} = {{isa = XCBuildConfiguration; name = Release; buildSettings = {{SDKROOT = macosx; }}; }};',
        f'''{config} = {{isa = XCBuildConfiguration; name = Release; buildSettings = {{
            PRODUCT_NAME = SocietyDriveFS; PRODUCT_BUNDLE_IDENTIFIER = com.iisacc.society.container.drive.fskit;
            INFOPLIST_FILE = "{filesystem / 'Info.plist'}";
            CODE_SIGN_ENTITLEMENTS = "{filesystem / 'SocietyDriveFS.entitlements'}";
            CODE_SIGN_STYLE = Automatic; CODE_SIGN_IDENTITY = "Apple Development"; DEVELOPMENT_TEAM = "{team}";
            MACOSX_DEPLOYMENT_TARGET = 26.0; SWIFT_VERSION = 5.0; SWIFT_OPTIMIZATION_LEVEL = "-O";
            ENABLE_HARDENED_RUNTIME = YES; SKIP_INSTALL = YES; }}; }};''',
    ]
    (project / "project.pbxproj").write_text("// !$*UTF8*$!\n{archiveVersion = 1; objectVersion = 56; classes = {}; objects = {\n" +
                                         "\n".join(objects) + f"\n}}; rootObject = {root}; }}\n")
    run("xcodebuild", "-quiet", "-project", str(project), "-target", "SocietyDriveFS", "-configuration", "Release",
        "-allowProvisioningUpdates", f"SYMROOT={output / 'fskit-products'}", f"OBJROOT={output / 'fskit-intermediates'}")
    destination = app / "Contents/Extensions/SocietyDriveFS.appex"
    shutil.copytree(output / "fskit-products/Release/SocietyDriveFS.appex", destination, dirs_exist_ok=True)
    shutil.copy2(filesystem / "LICENSE.txt", app / "Contents/Resources/FSKit-LICENSE.txt")



def build_mount_host(source, shared, output, app, team):
    """Provision the macOS 27 public mounting capability for the native host."""
    if not team:
        return None
    project = output / "SocietyContainerDrive.xcodeproj"
    project.mkdir(exist_ok=True)
    entitlements = output / "mount-host.entitlements"
    entitlements.write_bytes(plistlib.dumps({"com.apple.developer.fskit.mount": True}))
    info = output / "mount-host-Info.plist"
    shutil.copy2(app / "Contents/Info.plist", info)
    files = [FILE_PROVIDER_SWIFT, shared / "LocalDriveStore.swift", source / "DirectoryMount.swift", source / "DriveHost.swift"]
    refs, builds, objects = [], [], []
    for index, path in enumerate(files, 20):
        ref, build = f"{index:024X}", f"{index + 100:024X}"
        refs.append(ref); builds.append(build)
        objects += [f'{ref} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.swift; path = "{path}"; sourceTree = "<absolute>"; }};',
                    f'{build} = {{isa = PBXBuildFile; fileRef = {ref}; }};']
    ids = [f"{number:024X}" for number in range(1, 11)]
    root, group, product, target, phase, configs, config, project_configs, project_config, products = ids
    objects += [
        f'{root} = {{isa = PBXProject; mainGroup = {group}; productRefGroup = {products}; buildConfigurationList = {project_configs}; targets = ({target},); compatibilityVersion = "Xcode 14.0"; }};',
        f'{group} = {{isa = PBXGroup; children = ({",".join(refs)}, {products},); sourceTree = "<group>"; }};',
        f'{products} = {{isa = PBXGroup; children = ({product},); name = Products; sourceTree = "<group>"; }};',
        f'{product} = {{isa = PBXFileReference; explicitFileType = "wrapper.application"; path = Society.app; sourceTree = BUILT_PRODUCTS_DIR; }};',
        f'{target} = {{isa = PBXNativeTarget; name = SocietyContainerDrive; productName = Society; productReference = {product}; productType = "com.apple.product-type.application"; buildConfigurationList = {configs}; buildPhases = ({phase},); buildRules = (); dependencies = (); }};',
        f'{phase} = {{isa = PBXSourcesBuildPhase; files = ({",".join(builds)},); buildActionMask = 2147483647; runOnlyForDeploymentPostprocessing = 0; }};',
        f'{configs} = {{isa = XCConfigurationList; buildConfigurations = ({config},); defaultConfigurationName = Release; }};',
        f'{project_configs} = {{isa = XCConfigurationList; buildConfigurations = ({project_config},); defaultConfigurationName = Release; }};',
        f'{project_config} = {{isa = XCBuildConfiguration; name = Release; buildSettings = {{SDKROOT = macosx; }}; }};',
        f'''{config} = {{isa = XCBuildConfiguration; name = Release; buildSettings = {{
            PRODUCT_NAME = Society; EXECUTABLE_NAME = SocietyContainerDrive;
            PRODUCT_BUNDLE_IDENTIFIER = com.iisacc.society.container.drive;
            INFOPLIST_FILE = "{info}"; CODE_SIGN_ENTITLEMENTS = "{entitlements}";
            CODE_SIGN_STYLE = Automatic; CODE_SIGN_IDENTITY = "Apple Development"; DEVELOPMENT_TEAM = "{team}";
            MACOSX_DEPLOYMENT_TARGET = 15.0; SWIFT_VERSION = 5.0; SWIFT_OPTIMIZATION_LEVEL = "-O";
            ENABLE_HARDENED_RUNTIME = YES; SKIP_INSTALL = YES; OTHER_LDFLAGS = "-framework Foundation -framework CryptoKit -framework FSKit -framework DiskArbitration -framework FileProvider";
        }}; }};''',
    ]
    (project / "project.pbxproj").write_text("// !$*UTF8*$!\n{archiveVersion = 1; objectVersion = 56; classes = {}; objects = {\n" +
                                         "\n".join(objects) + f"\n}}; rootObject = {root}; }}\n")
    run("xcodebuild", "-quiet", "-project", str(project), "-target", "SocietyContainerDrive", "-configuration", "Release",
        "-allowProvisioningUpdates", f"SYMROOT={output / 'host-products'}", f"OBJROOT={output / 'host-intermediates'}")
    built = output / "host-products/Release/Society.app"
    shutil.copy2(built / "Contents/MacOS/SocietyContainerDrive", app / "Contents/MacOS/SocietyContainerDrive")
    shutil.copy2(built / "Contents/embedded.provisionprofile", app / "Contents/embedded.provisionprofile")
    # Preserve Xcode's application/team identifiers when sealing the assembled
    # host with its File Provider and filesystem extensions.
    signed = subprocess.check_output(["codesign", "-d", "--entitlements", ":-", str(built)], stderr=subprocess.DEVNULL)
    actual = output / "mount-host-signed.entitlements"
    actual.write_bytes(plistlib.dumps(plistlib.loads(signed)))
    return actual


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--catalog-tool", required=True)
    parser.add_argument("--sign", default="-")
    parser.add_argument("--version", required=True)
    parser.add_argument("--tests", action="store_true")
    parser.add_argument("--fskit-team", default="")
    args = parser.parse_args()
    source = pathlib.Path(__file__).resolve().parent
    shared = source.parent / "apple"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    catalog = subprocess.check_output([args.catalog_tool, "catalog"])
    (output / "Sections.json").write_bytes(catalog)
    app = output / "Society.app"
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
                    CFBundleName="Society", CFBundleDisplayName="Society",
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
                "-framework", "Foundation", "-framework", "CryptoKit", str(FILE_PROVIDER_SWIFT)]
    run(*compiler, "-module-name", "SocietyContainerProvider", "-parse-as-library", "-emit-executable",
        "-Xlinker", "-e", "-Xlinker", "_NSExtensionMain", "-framework", "FileProvider",
        "-framework", "UniformTypeIdentifiers", "-framework", "CoreServices",
        str(shared / "LocalDriveStore.swift"), str(shared / "FilesDriveStore.swift"),
        str(shared / "FileProviderExtension.swift"),
        "-o", str(provider / "Contents/MacOS/SocietyContainerProvider"))
    run(*compiler, "-framework", "FileProvider", "-framework", "FSKit", "-framework", "DiskArbitration", str(shared / "LocalDriveStore.swift"),
        str(source / "DirectoryMount.swift"), str(source / "DriveHost.swift"),
        "-o", str(app / "Contents/MacOS/SocietyContainerDrive"))
    entitlements = output / "provider.entitlements"
    entitlements.write_bytes(plistlib.dumps({
        "com.apple.security.app-sandbox": True,
        "com.apple.security.files.user-selected.read-write": True,
        "com.apple.security.files.bookmarks.app-scope": True,
    }))
    run("codesign", "--force", "--timestamp=none", "--sign", args.sign,
        "--entitlements", str(entitlements), str(provider))
    build_filesystem(source, output, app, args.fskit_team)
    host_entitlements = build_mount_host(source, shared, output, app, args.fskit_team)
    sign_host = ["codesign", "--force", "--timestamp=none", "--sign", args.sign]
    if host_entitlements:
        sign_host += ["--entitlements", str(host_entitlements)]
    run(*sign_host, str(app))
    if args.tests:
        run(*compiler, "-framework", "FSKit", "-framework", "DiskArbitration", str(shared / "LocalDriveStore.swift"), str(source / "DirectoryMount.swift"),
            str(source / "DirectoryMountTests.swift"), "-o", str(output / "DirectoryMountTests"))
        run(*compiler, str(source / "FSKit/SocietyFSMetadata.swift"), str(source / "FSKitMetadataTests.swift"),
            "-o", str(output / "FSKitMetadataTests"))
        run(*compiler, str(source / "FSKit/SocietyFSFileDescriptor.swift"), str(source / "FSKitIOTests.swift"),
            "-o", str(output / "FSKitIOTests"))
        run(*compiler, str(source / "FSKit/SocietyFSDirectory.swift"), str(source / "FSKitDirectoryTests.swift"),
            "-o", str(output / "FSKitDirectoryTests"))
        run(*compiler, str(source / "FSKit/SocietyFSStat.swift"), str(source / "FSKitStatTests.swift"),
            "-o", str(output / "FSKitStatTests"))
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
