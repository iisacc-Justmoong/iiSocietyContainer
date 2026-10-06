> The actual APFS disk of the new Society onboarding uses [DiskImage](../../docs/DiskImage.md).
> This document describes the Files-specific projection adapter for the existing File Provider.

# macOS Society

Apple's [replicated File Provider](https://developer.apple.com/documentation/fileprovider/replicated-file-provider-extension)provides Society drive to Finder and file dialogs. Minimum macOS 15. Foundation, FileProvider, UniformTypeIdentifiers, CryptoKit, CoreServices are frameworks included in the operating system, adding no FUSE, external drivers, or paid cloud services. The Swift compiler uses Xcode Command Line Tools, and bundle creation uses Python 3. Apple framework usage conditions follow Apple SDK license.

<a id="빌드와-연결"></a>

## Build and Link

Execute from SDK root. The default signature is ad hoc, and the build for actual Finder use is configured with the developer's valid code signing ID. The signing ID is not pinned to the repository.

```sh
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH="/Volumes/Storage/Qt/6.8.3/macos" \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install" \
  -DIISOCIETYCONTAINER_SIGNING_IDENTITY="<your code signing identity>"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build
```

Society The app's `Connect to Finder` registers installed adapters and connects the File Provider domain to the SDK drive ID. For the first connection, Finder's standard `Enable` behavior may be required. The path of the original folder and the path managed by the system are different. CLI connection is as follows.

```sh
build/iiSocietyContainerDriveTool create "/path/to/existing/source"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
  -f "$PWD/build/native/Society.app"
pluginkit -a "$PWD/build/native/Society.app/Contents/PlugIns/SocietyContainerProvider.appex"
open -W -g -a "$PWD/build/native/Society.app" --args list
"build/native/Society.app/Contents/MacOS/SocietyContainerDrive" register "/path/to/existing/source"
"build/native/Society.app/Contents/MacOS/SocietyContainerDrive" list
"build/native/Society.app/Contents/MacOS/SocietyContainerDrive" path "<drive-id>"
"build/native/Society.app/Contents/MacOS/SocietyContainerDrive" refresh "<drive-id>"
"build/native/Society.app/Contents/MacOS/SocietyContainerDrive" unregister "<drive-id>"
```

The host returns a termination code instead of 0 on failure. Repeated registration uses the same domain and updates the display name. The `enabled` in the response indicates the domain registered by the system again. Duplicate connections with the same ID but different original paths are rejected. Connection cancellation preserves downloaded data with `preserveDownloadedUserData` and returns the preservation path. The original is not deleted. The local development bundle does not include timestamps, notarization, or deployment packages for other computers.

<a id="파일-계약"></a>

## File Contract

Original selection is validated by `SocietyDrive` and native `LocalDriveStore`. `~/Library/CloudStorage/` Finder copies and their subfolders, and the interior of other Society containers cannot be created, opened, or registered as originals. Symbolic links are resolved to actual paths and do not create sections, manifests, or lock files before rejection. `SharedStorage` and Helper also use the same validation, so incorrect originals cannot be registered as common storage paths. If a copy is designated as the original and `Society/Files/` contains 8 nested areas, the file is preserved and moved to the corresponding area of the original, and the original path must be reselected in Society.

Folders and files of 8 areas are stored in the original directory. System copies provide only `Society/Files/`. The drive's `/Example.txt` is the original's `Society/Files/Example.txt`, and files and folders can be created directly at the drive root. Within this scope, read, create, content modification, name change, move, and delete operations are reflected in the original, and external changes from `Files/` are notified to the working set via recursive FSEvents monitoring. Both the original and system cache can use disk space. If the original disk is disconnected, an access error is returned.

Asset Library, Deleted, Forked, Generation History, Models, Published, and Thinking Space are navigated in the Society app. The system drive does not list folders and files of these areas, and lookup, download, creation, move, modification, and deletion using IDs of previous versions are also rejected. Internal `section:files` IDs are not disclosed. Name change, move, and deletion of the drive root itself are not allowed, while root metadata notifications due to child changes are accepted. If a user creates a general folder named `Models` inside the drive, it becomes `Society/Files/Models`, and thus is distinguished from the app's `Society/Models` area.

This boundary applies to access via File Provider. It is not an encryption or isolation feature that changes file permissions of the original folder or blocks access to the original path of the same OS user. The Society app provides all 8 areas from the original. `Deleted` is not connected to the system trash. Unsorted files, symbolic links, and special files in the existing root are not exposed to the system drive. Separate metadata synchronization such as Finder tags, extended attributes, and resource forks, as well as remote cloud synchronization, are not implemented.

File ID and change anchor are stored in the original's `.society-drive-provider.json`. Track moves with inode and creation time, and maintain path ID for atomic content replacement. If the moved original remains, a new file created in the old path receives a new ID. For anchors outside the recent 8 snapshots, request a full re-enum. If directory enumeration fails, do not process the partial result as a delete list. Since the current index scans the entire area, performance validation for large file sets is separate.

Public anchors use the `files-v5:` prefix. Protection of the default Files folder has been removed, so connections using an earlier prefix refresh their capability lists through full re-enumeration. Updating from a 0.4.0 anchor preserves existing Files children's IDs and moves them to the public root; the former 8 section folders and private items are placed in the system replica's deletion list. The previous full snapshot is used only for change comparison, and not for current item enumeration or downloads. Unsupported anchor formats request full re-enumeration. Items moved out of Files in the Society app disappear from the system, while items moved into Files become public. Previously copied data elsewhere, or data preserved when disconnecting, is not reclaimed.

The working set also includes the public root's metadata and delivers it together when the root changes. This also removes root-write restrictions left in existing connections, allowing Finder's `New Folder`. Visible enumeration of the root directory returns only its children. Content versions track file-content changes, while metadata versions track only the name and parent. A move requested immediately after editing content is not treated as a conflict because of a modification-time change. A directory's content version is fixed to the directory's own identifier, and children are tracked by each item's version, so creating or deleting children does not cause folder deletion to be rejected. Change notifications follow [Apple's File Provider change-tracking contract](https://developer.apple.com/documentation/fileprovider/tracking-your-file-provider-s-changes).

The host passes bookmarks including temporary permissions to the extension. The extension re-saves them as a security-scoped bookmark in its own process for use in the next execution. It does not reuse app-scoped bookmarks from other apps as-is. [Refer to Apple's inter-process bookmark passing method](https://developer.apple.com/documentation/browserenginekit/accessing-files-in-browser-extensions). It does not require sandbox exceptions or full disk access permissions.

<a id="구현과-검증"></a>

## Implementation and Verification

- `LocalDriveStore.swift`: Source validation, file operations, persistent ID, and change snapshots.
- Validation of the parent path of the original also iterates over the bookmark URL as a finite path component. It does not repeat the top URL operation that maintains the base URL of the bookmark, and it includes regression tests for general and security scope bookmarks.
- `FilesDriveStore.swift`: Projects Files as the public root, blocks private IDs and change targets, and converts public versions and previous anchors.
- `FileProviderExtension.swift`: File item, enumeration, content transfer, change callback, source monitoring
- `DriveHost.swift`: Register, View, Disconnect, Folder Access Permission Transfer
- : `build_native.py` : C++ Catalog creation, host and extended compilation, bundle configuration, signing
- `LocalDriveStoreTests.swift`: Verifies the same 8-section operations, file CRUD, reopening, move/replacement ID, version conflicts, anchors, root protection, and path traversal in a temporary directory.
- `FilesDriveStoreTests.swift`: Direct exposure of Files root, denial of access to private areas and preservation of originals, root CRUD, app area access, public suspension and re-publication, removal of existing exposure of actual enumeration callbacks and root permission refresh verification

Test temporary files and build results are placed under the `build/` of the SDK. Actual File Provider domains and bookmarks and cache are created at macOS in the system management location. General CTest does not automatically register the domain. Actual Finder behavior is separately checked in the explicitly connected verification folder.

Since 0.6.0, storage, public-boundary, and File Provider implementations in `platform/apple/` are shared with iOS. Selected paths, security-scoped bookmarks, and FSEvents behavior on macOS are retained. Index transactions are coordinated, and the latest on-disk history is read to prevent ID conflicts between separate storage instances. `shared_location` and `ios_package_contract` also run on the host, but are not treated as actual iOS device-verification results.

After updating the signed helper, the helper app must be one time via Launch Services to be recognized as belonging to an executable extension app. Society's connection flow performs `open -W -g -a ... --args list` after registration and connects the domain. If `NSFileProviderError -2001` and internal `-2014` occur by skipping this step, app registration and normal execution status are checked first.
