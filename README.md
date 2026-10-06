# iiSocietyContainer

0.14.0 separates the original file and the host's repository list. `StorageMap` reads the logical key, version, size, hash, and local hold status of `.society-sync/catalog.json`, and atomically records a download request to fix a specific version at `.society-sync/requests/`. It does not create an empty original file for empty items. `available(keys)` must be true and the request status must be `ready` before using the file. It rejects path escapes, redirects, and lists from other containers.

`StorageDirectoryModel` merges the actual directory and the host list in the worker thread to provide name, size, preview, and hold status. It selects only the immediate children of the current folder to validate the path, and reuses the parsing result for unchanged repository maps. `setFolder()` and `refresh()` do not wait for the GUI call and discard the late results of the previous folder. Empty folder URLs stop the lookup timer. `activate` requests the non-held file and sends `activated` after the verified download completes. `StorageModelCatalog` constructs the model card with name, path, format, size, and hold status without reading the original header or package if the host catalog exists. Catalog monitoring is limited to two directories, and local-only fallback monitoring is limited to a maximum of 64 items. `activatePath()` passes only the selected file or package as a version-fixed download request to the SDK and may send `objectReady` after completion. It maintains the structural judgment and import validation for local-only repositories. `SharedStorage::models()` also displays non-held models, and `resolveModel()` succeeds only when all required files are ready. The model reference of a package is fixed to the version of the entire configuration file. `shared_storage` tests check only-list models, request cancellation, opening after navigation and download completion without file creation.

`Files/` starts without default files or folders. Automatic creation and name reservation/deletion protection for Documents, Audios, 3D objects have been removed. Only existing empty default folders are one time cleaned up, preserving user content and folders created directly afterward. Refer to [Files contract](docs/Files.md). Photos is an independent top-level area.

The drive's display name is `Society`. Since 0.9.1, new manifests also record this name. Existing `Society Container` manifests open without modifying their UUIDs or files and are displayed as `Society`. Read boundaries in C++, Apple, and Android accept both names. The Windows volume name, Linux FUSE name, Android document root, and Apple File Provider root also use the same display name.

macOS re-invokes `register` with the existing original, updating the display name with the same File Provider domain ID. The domain is not removed. The installed Apple SDK's `NSFileProviderManager.addDomain` contract is used, following [Apple domain API](https://developer.apple.com/documentation/fileprovider/nsfileprovidermanager/add(_:completionhandler:)). The adapter bundle name is also changed to `Society.app` to display the same name in the Finder sidebar. CMake's `iiSocietyContainer_NATIVE_APP` provides the new path, and the executable/bundle ID and Windows auto-launch registration key are kept as internal identifiers for existing integration.

`iiSocietyContainer.drive` and `native_store` verify the new name, compatibility with the previous name, UUID, and preservation of the existing manifest. iOS package tests and Android provider tests check the name displayed on the system.

Version 0.14.0 libraries using C++23 and Qt 6.8.3 Core (desktop·Android dynamic, iOS static) are. `DiskImage` creates a separate APFS disk image at the archive location and mounts it. The existing directory-based API is a low-level compatibility API for the logical layout. `SocietyDrive` consists of a persistent drive ID and 9 areas. macOS · iOS File Provider, Windows Dokan, Linux FUSE, Android DocumentsProvider provide the contents of `Files/` to the system drive root. Society and iisacc app use internal 9 areas, and other Android apps use the same signed URI and Helper file system API.

`Models/` provides 23 model type folders. `ModelStore` is responsible for listing, auto-classification, manual type modification, and path interpretation, while `ModelClassifier` reads limited metadata. 0.11.1's `metadata()` queries the architecture, precision, and explicit media classification to be displayed on the consumer app's cards. 0.11.2 adds Anima checkpoint structure validation and Safetensors tensor dimension, data type, and byte range verification. Creation, legacy cleanup, judgment rules, move history, and CLI contracts are described in the [Models management document](docs/Models.md).

<a id="공개-api"></a>

## Public API

Version 0.15.0 adds live file metadata, immediate directory children, and recursive tree snapshots through `SocietyContainer::entry/entries/tree` and `SocietyDrive::entry/entries/tree`. Nodes expose native and relative paths, parent relationships, file type, size, modification time, permissions, loaded children, and lookup with `find()`. Depth and node limits keep traversal explicit. Drive snapshots preserve all nine logical sections and correctly map a separate Files volume. The standalone `iiSocietyContainer::FileTree` C++23 target has no Qt dependency. Directory creation and collision-safe moves extend `FileOperations`; see the [file tree API and examples](docs/FileTree.md).

`SocietyDrive::create()` and `open()` do not accept a location inside another Society container as the source. On macOS, Finder replicas under `~/Library/CloudStorage/` are also rejected. Paths are resolved to their actual locations and validated before file creation, preventing 8 app areas from being recreated inside `Files/`. Shared storage settings and Helper file-system access apply the same boundary. The source retains 9 areas, while the system drive exposes only the contents of the source `Files/`. Recovery procedures are described in the [macOS file contract](platform/macos/README.md#파일-계약).

```cpp
#include <iiSocietyContainer.h>

using iiSocietyContainer::SocietyContainer;
using PathKind = SocietyContainer::PathKind;

const SocietyContainer container(QStringLiteral("/data/MyLibrary"));
if (!container.isValid()) {
    qWarning("%s", qPrintable(container.errorString()));
    return;
}

const QString root = container.rootPath(); // Normalized absolute directory path
const auto kind = container.classifyPath(QStringLiteral(".")); // PathKind::Root
const auto asset = container.classifyPath(QStringLiteral("assets/model.bin"));
// If the actual file exists within the root, this is PathKind::Entry.
const auto outside = container.classifyPath(QStringLiteral("../OtherLibrary"));
// PathKind::Outside
```

`SocietyContainer` generator requires a non-empty actual file system directory path. Relative paths passed to the generator are interpreted based on the working directory at the time of creation. Subsequently, the root is fixed as a normalized absolute path, so even if the working directory changes, it points to the same space. Names with spaces and Unicode are preserved as is. Paths containing files, non-existent directories, NUL characters, and Qt resource paths are rejected, and upon generation failure, `isValid()` returns `false`, `rootPath()` returns an empty string, and `errorString()` returns an error description.

Relative paths passed to `classifyPath()` are interpreted based on the root of this space. The judgment results are as follows.

|Result|Meaning|
| --- | --- |
| `PathKind::Root` |The specified SocietyContainer directory itself|
| `PathKind::Entry` |A file or directory that actually exists under the root|
| `PathKind::Outside` |External paths, non-existent items, invalid input, or invalid containers|

`.` is the root and an empty string is invalid input. Symbolic links are judged based on the actual target path. Links pointing from inside to outside are `Outside`, and links pointing from outside to inside items are `Entry`. Paths that go beyond the root with `..` and side directories with only the same prefix like `MyLibrary-backup` are also `Outside`. If the root itself is specified as a link, it is fixed to the initial target directory, and changing the original link to another location does not change the space's root.

Space specification is stored in the object. It does not create directories, rename names, or record marker files, nor does it modify existing content. To use in a separate execution, the object must be reconstructed with the same path. Since `isValid()` and path validation check the current file system, they become invalid if the specified root disappears or is replaced by a link pointing to a different location. Since it is based on the path, it becomes valid again if an actual directory is recreated at the same location. This is a path classification API, and it does not provide access control or file system isolation features that block concurrent file system changes.

Existing `iiSocietyContainer::helloWorld()` continues to return `Hello world!` for consumer compatibility. The container's public header and source are kept together in the project root, while the domain model's header and source are kept together in `src/Store/`. The public API uses platform-specific export/import macros for common `iiSocietyContainerExport.h`. CMake targets `iiSocietyContainer::iiSocietyContainer` and delivers C++20 requirements and Qt Core dependencies to the consumer.

<a id="논리-영역"></a>

## Logical Domain

`iiSocietyContainer::StoreSection` enum clearly distinguishes domains. The domain's unique identifier is the enum value, and the string returned by `storeSectionName()` is the display name. The domain list is fixed in the following order.

|identifier|display name|
| --- | --- |
| `StoreSection::AssetLibrary` | Asset Library |
| `StoreSection::Deleted` | Deleted |
| `StoreSection::Files` | Files |
| `StoreSection::Forked` | Forked |
| `StoreSection::GenerationHistory` | Generation History |
| `StoreSection::Models` | Models |
| `StoreSection::Published` | Published |
| `StoreSection::ThinkingSpace` | Thinking Space |

```cpp
using iiSocietyContainer::StoreSection;
using iiSocietyContainer::storeSectionName;

const auto sections = container.sections(); // 9 sections for a valid container
for (const StoreSection section : sections) {
    const QString name = storeSectionName(section);
    // Identify the section with section and display name on screen.
}

const bool hasModels = container.hasSection(StoreSection::Models);
```

`allStoreSections()` returns a list of 9 areas regardless of container status. `container.sections()` and `container.hasSection()` reflect the current validity of the container. If the container is invalid, it returns an empty list and `false` respectively. For undefined enum values, `storeSectionName()` returns an empty string and `hasSection()` returns `false`.

region lookup for `SocietyContainer` is responsible only for identification and separation. Specifying an empty directory returns 9 regions, but does not create physical directories or modify existing files through lookup alone. All regions are provided in the same manner, without defining data formats, retention policies, or permissions per region. `classifyPath()` maintains the existing container boundary determination. The actual directory layout of the drive is explicitly initialized in `SocietyDrive::create()`.

Region definition and name list are handled by `src/Store/StoreSection.h` and `src/Store/StoreSection.cpp`. This sub-module does not reference container implementation, while the upper-level `SocietyContainer` uses the region model.

<a id="드라이브"></a>

## drive

```cpp
#include <SocietyDrive.h>

QString error;
auto drive = iiSocietyContainer::SocietyDrive::create("/data/MyLibrary", &error);
if (!drive) {
    qWarning("%s", qPrintable(error));
    return;
}
const QString id = drive->identifier();
const QString files = drive->sectionPath(iiSocietyContainer::StoreSection::Files);
auto reopened = iiSocietyContainer::SocietyDrive::open("/data/MyLibrary", &error);
// reopened->identifier() == id
```

`create()` creates subdirectories matching the 9 section names exactly, along with `.society-drive.json`, inside an **already existing directory**. Existing valid section directories and their contents are preserved. A file, symbolic link, or junction with the same name, or an invalid existing manifest, returns an error without being overwritten. Initialization locking and atomic manifest saving are used. On failure, only empty directories created by this call are cleaned up. A valid existing drive is opened with the same ID. `open()` creates no files; it validates the manifest, version, ID, and complete section layout.

The manifest version is `schemaVersion: 1`, its type is `type: "SocietyDrive"`, and its display name is `Society`. The identifier is a UUID, and section entries contain `id`, `name`, and `path`. The persistent keys returned by `storeSectionKey()` are, in order, `asset-library`, `deleted`, `files`, `forked`, `generation-history`, `models`, `published`, and `thinking-space`. Display names and directory names are as listed in the table above. The native section catalog is generated from the `catalog` output of the C++ tool rather than defined redundantly.

`sectionForPath()` returns regions based on normalized targets of paths that actually exist. Root itself, root items outside regions, missing items, and external paths are `std::nullopt`. Unclassified files of the existing root are preserved and do not arbitrarily move to any region. All regions including `Deleted` are the same general folder, and business rules such as trash, publish, or creation history are not yet assigned.

On macOS 15 or later, the native adapter is built alongside the SDK. It is installed at `share/iiSocietyContainer/Society.app` and exposed through `iiSocietyContainer_NATIVE_APP` in the CMake package. The SDK's `iiSocietyContainerDriveTool create|open <path>` exposes the same API through a CLI. Finder registration, signing, file reflection behavior, and scope follow the [macOS adapter documentation](platform/macos/README.md). [Windows/Linux mounting](platform/desktop/README.md)and [Android document-provider and inter-app sharing](platform/android/README.md)are defined in separate platform documentation.

Opening or saving `Example.txt` from the system drive corresponds to `Files/Example.txt` of the original. A separate `Files` folder stage is not indicated. The remaining 7 regions and container metadata are excluded from the system drive's list/search working set and file ID access, and are provided via Society app's region navigation. This is the exposure scope of File Provider and does not change the operating system permissions or encryption of the original directory.

<a id="네이티브-디스크-이미지"></a>

## native disk image

Society desktop onboarding creates an ordinary `Society/` directory with `SocietyDrive::createAt(parent)`. The nine Storage sections are direct child directories, including `Files` and `Models`; the file manager opens the complete root. Shared settings preserve the directory path and container UUID without mounting a disk image. Existing sparsebundle images remain readable for recovery and migration. See [directory storage and migration](docs/DirectoryStorage.md) and the [legacy disk-image API](docs/DiskImage.md).

<a id="iisacc-공통-스토리지"></a>

## iisacc Common Storage

`SharedStorage.h/.cpp` provides repository discovery independent of app name, model list and reference interpretation, and area internal directory creation. Society registers the selected source, and Dreamscapes and consumers open the same repository. App-specific model copies or Finder's `Files/` projection are not used.

Generation queue, prompt, and execution status are stored in each app instance's memory and are not saved to Society or restored upon re-execution. Temporary materials requiring file paths from the generator are processed in the app-specific temporary directory outside Society and cleaned up when the task is finished. Only completed image files are saved immediately below `Generation History/`, and no app-specific or task-specific folders or automatic Asset Library registration are created. `SharedStorage` does not own the generation queue or temporary task lifecycle.

```cpp
#include <SharedStorage.h>
using namespace iiSocietyContainer;
QString error;
SharedStorage::setDefaultContainer("/data/Society", &error); // Existing drive selected by Society
auto storage = SharedStorage::open({}, &error);             // Another iisacc app
if (storage) {
    const auto models = storage->models(&error);
    if (!models.isEmpty()) {
        const auto reference = models.first().reference(storage->drive().identifier());
        // Keep the reference in the request in app memory and resolve it again immediately before execution.
        const auto originalWeights = storage->resolveModel(reference, &error);
    }
    const auto image = storage->filePath(StoreSection::GenerationHistory, "unique-result.png", &error);
}
```

On desktop, `QStandardPaths::GenericConfigLocation/iisacc/Society/storage.json` atomically stores the original path and drive UUID. `open()` priority is explicit path, `SOCIETY_CONTAINER_PATH`, and common settings. Test and isolated execution change the config file to the absolute path `SOCIETY_STORAGE_SETTINGS_PATH`. If the drive UUID of the set path changes, it returns an error without arbitrary switching, asking Society to select again. This is local repository discovery for the same user environment and does not implement account authentication, network synchronization, or remote inference.

The model list enumerates the Diffusers directory including `Models/` `.safetensor` · `.safetensors` files (case-insensitive) and `model_index.json`. Individual weights inside the package are not redundantly listed as separate models. Hidden items, symbolic links, and paths going outside the boundary are excluded. This list is a candidate list of storage formats and does not confirm the type of Diffusion · LLM, the role of checkpoint · LoRA, or executability. Model meaning and whether inference support is available are verified by the engine of the consuming app.

A reference is `{containerId, path, format, fingerprint}`. `path` is a path relative to `Models/`, and `fingerprint` is a change-detection value based on each file's relative path, size, modification time, and first 64 KiB. It is neither a hash of all weights nor an immutable snapshot. References to another drive or to deleted, redirected, or modified models are rejected; full weight provenance is recorded by the actual generation engine. `ensureDirectory()` creates only relative paths beneath the specified section and rejects `..`, `.`, empty intermediate components, backslashes, colons, NUL, and redirection. General file-system access also supports hidden files and directories, while model discovery continues to exclude hidden entries. This is not an isolation mechanism that blocks concurrent file-system changes.

iOS opens the `Library/Application Support/Society` of the same App Group as Society instead of Documents per app. The consuming app sets the bundle ID and then calls `iiSocietyContainer_configure_ios_client(Dreamscapes APP_GROUP group.com.iisacc.society DISPLAY_NAME Dreamscapes TEAM ...)`. This function configures the same `SocietyAppGroup` Info.plist key and App Group entitlement while maintaining the consuming app name and bundle ID, without adding a separate File Provider extension. Society must be opened first to initialize the original. All participating apps must be signed with a valid group entitlement from the same Apple team. [](https://developer.apple.com/documentation/xcode/configuring-app-groups)uses the shared container API.

Common storage tests and install consumers verify that the same UUID is found even if the app name differs, configured drive replacement detection, original model path parsing, package duplication exclusion, model change detection, and internal output directory boundaries. iOS Package inspection checks the actual generated plist and group entitlement match between Society and Dreamscapes, without substituting device execution.

0.8.0's `SharedStorage::filePath(section, relativePath, error)` returns an absolute path for regular file I/O. An empty relative path returns a new file path even if only the last name exists, and the parent is prepared first via `ensureDirectory()`. Path lookup does not create files. Each request validates the original path and UUID and path elements, rejecting symbolic links and junctions at root and subpaths, and domain exit. The feature to lock even concurrent changes after return is not provided. iiSocietyHelper 0.4.0's `fileSystem` reuses this API. Common storage tests and install consumers check 9 domains for regular read/write and boundary/original replacement.

<a id="의존성-검토"></a>

## Dependency Review

Path lookup and normalization use `QFileInfo` and `QDir` from the existing Qt Core. Because they use [Qt's path-normalization API](https://doc.qt.io/qt-6.8/qfileinfo.html#canonicalFilePath)and [relative-path API](https://doc.qt.io/qt-6.8/qdir.html#relativeFilePath), no separate file-system library is needed. Logical sections are a list of identifiers specific to this SDK, represented using only the C++ standard library and `QList`/`QString` from the existing Qt Core. With no additional external dependencies, runtime dependencies and licensing scope remain the same as for the existing Qt Core. Tests use Qt Test from the same Qt 6.8.3 distribution, which is not included in the SDK's public dependencies.

<a id="빌드-테스트-설치"></a>

## Build, test, install

CMake, 3.24 or higher, C++20 compiler, Qt **6.8.3** Core development files are required. When building tests, Qt Test development files are also required. macOS searches for `/Volumes/Storage/Qt/6.8.3/macos` by default.

```sh
./install.sh
```

In a standalone build, the default install path is applied, and when included in a parent CMake project, the parent project's install path is maintained. The script runs Release configuration, build, and CTest at `build/` and installs to `$HOME/.local/SDK/iiSocietyContainer`. Then, a separate project configured at `build/consumer/build/` consumes only the installed CMake package and runs build and CTest. The original build and install consumer each run the persona function compatibility test and directory space test. To verify within the workspace without SDK install, use `INSTALL_PREFIX="$PWD/build/install" ./install.sh`.

Tests verify C++20 and Qt versions, root specification, internal and external verdicts, relative path and working directory changes, independent space, invalid input, whitespace and Unicode names, folder content preservation, and root deletion. On Unix systems, symbolic link interpretation, external exit, broken links, and root replacement are also verified. On Windows, `QFile::link` creates shortcuts, so the symbolic link-only test is skipped. The temporary test directory is created and cleaned up below the test execution directory.

Logical area tests verify 8 identifiers and names, order, full area provision in empty containers, independence from physical file configuration, file system preservation during query, and handling of invalid containers and incorrect area values. Install consumers also independently include area headers and run the same test.

Settings are specified as environment variables instead of command-line arguments. `CMAKE_PREFIX_PATH` is an additional CMake search path separated by semicolons.

```sh
INSTALL_PREFIX="$HOME/.local/SDK/iiSocietyContainer" \
QT_PREFIX_PATH="/Volumes/Storage/Qt/6.8.3/macos" \
CMAKE_PREFIX_PATH="/additional/prefix" \
./install.sh
```

Manual execution is also possible. All build outputs are placed under `build/`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DCMAKE_PREFIX_PATH="/Volumes/Storage/Qt/6.8.3/macos" \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/SDK/iiSocietyContainer"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release
cmake -S tests/consumer -B build/consumer/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/.local/SDK/iiSocietyContainer;/Volumes/Storage/Qt/6.8.3/macos"
cmake --build build/consumer/build --config Release --parallel
ctest --test-dir build/consumer/build -C Release --output-on-failure
```

<a id="설치-패키지-사용"></a>

## Use install package

```cmake
find_package(iiSocietyContainer 0.6.0 CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE iiSocietyContainer::iiSocietyContainer)
```

When configuring consumers, add SDK install path and Qt path to `CMAKE_PREFIX_PATH`. The default install configuration is as follows.

- `include/iiSocietyContainer.h`: Public header
- `include/iiSocietyContainerExport.h`: Common export/import macro
- `include/SocietyDrive.h`: Persistent drive API
- `include/src/Store/StoreSection.h`: Public header of logical area that can be included independently
- `lib/`: Shared library with version; Windows runtime DLL is `bin/`
- `lib/cmake/iiSocietyContainer/`: Config, ConfigVersion, and Targets package
- `share/iiSocietyContainer/README.md`: This document
- `bin/iiSocietyContainerDriveTool`: Drive initialization, query, and area catalog CLI
- macOS's `share/iiSocietyContainer/Society.app`: Native host and File Provider extension

Drive tests verify initialization, re-open, ID preservation, 8 folder count, original preservation on conflict, manifest errors, and area boundaries from the original and install consumer. macOS Native tests verify file operations on the entire original separately, `Files/` root exposure, non-public ID access denial, root file CRUD, version conflicts, app access hold, restart, and removal of existing 9 area exposures.

Actual Finder connection tests are performed on explicitly registered verification drives, separated from general CTest. Qt itself is not reinstalled or bundled. The execution environment must also have Qt 6.8.3 Core. Linked dependency paths reflect the install RPATH. Qt usage conditions follow the license of the existing Qt install.

## License

SPDX-License-Identifier: AGPL-3.0-only

Self-written code and documents of iiSocietyContainer are distributed exclusively under the GNU Affero General Public License v3.0. The full terms follow [LICENSE](LICENSE).

External libraries including Qt and third-party code with separate notices maintain their own licenses. This project's license declaration does not replace the corresponding third-party license.

<a id="ios--ipados-통합"></a>

## iOS / iPadOS integration

iOS 16 and above use the same App Group original for Society app and built-in File Provider extension. The app explores 9 areas, and the Files app directly exposes only Files content. Common Swift repository and public boundary are at `platform/apple/`, and iOS domain registration, bundle, permission, and install configuration are defined in [iOS document](platform/ios/README.md). iOS device and simulator-specific build presets are provided by Society app.

### Shared logical drive identity (0.10)

`SocietyDrive::adoptReplicaIdentity(root, expectedId, hostId)` atomically adopts
an authenticated host's UUID under the drive lock. The existing nine section
paths remain unchanged; stale drive objects become invalid and must be reopened.
iiSocietySync owns host selection, private recovery of a former independent
container, and initial mirroring before uploads. A device's replica journal and
credentials remain local. This API alone does not copy or merge files.

`completeReplica(root, hostId)` marks the host metadata snapshot ready. `isValid()`
checks structure and identity; `isReady()` additionally checks publication state.
The manifest retains `localIdentifier` for a stable native registration and
`previousIdentifier` for recovering the selected drive across another host-drive
change. Apple retains provider history and reopens the adopted identity; Android
reopens its cached store. Consumer `SharedStorage::open()` and file/model access
reject incomplete mirrors. The storage owner can resolve their location through
`open(path, error, true)` to resume initialization, while consumer operations on
that handle remain gated. No credentials or account models enter this manifest.

Photos path and manifest migration contract follows [Photos.md](docs/Photos.md). `FileDirectoryKind::Photos` maintains only value compatibility and does not return at FilesView. `StoreSection::Photos` is used.

<a id="공유-대시보드와-society-앱-연결"></a>

## Shared dashboard and Society app connection

`Gui` component's `DashboardFiles` is a read-only asynchronous list shared by Society and Dreamscapes. Specify a valid local Society drive at `containerPath` and use `recentFiles`, `recentPublished`, `generationHistory`. Files and Generation History are limited to the latest 20, and Published is limited to the latest 4 for the mobile dashboard. It provides full snapshot search, atomic image replacement monitoring, and list preservation on cancelled repository query abandonment and refresh without changes. The app is responsible only for QML registration and expression.

`SocietyApplication::openGenerationHistory()` delivers `society://generation-history` as the operating system and returns whether the execution request was accepted. Only Society hosts call `listen()` and connect `generationHistoryRequested` to the Storage screen. URLs do not accept file paths or account information, and other host·path·query parameters are not accepted. macOS / iOS support URL events, Android's Qt URL handling, and desktop execution arguments. URL registration is handled in the Society app package, and Windows registers it for the current user upon Society execution. Unit tests verify inclusion, exclusion, sorting, monitoring of the list, and URL delivery.

### Generation inventory reconciliation

`SharedStorage::models()` reconciles a local authority catalog with native Models files before returning generation models: Deleted moves disappear and new files/packages appear before background hashing finishes. Existing indexed version references remain compatible with remote generation. Discovery reads file metadata, not tensor payloads; hidden runtime directories and redirected paths are excluded. A replica with an empty catalog stays empty instead of exposing leftover local files. `resolveModel()` uses the same inventory. Regression coverage: `generationInventoryReconcilesAuthorityAndPreservesReplicas`.
# File loading performance

Application-local directory snapshots, SHA-256/preview caching and bounded parallel
loading are documented in [FileLoading.md](FileLoading.md). GUI consumers register
`iiSocietyContainer::PreviewProvider` under `society-preview` before loading QML.
