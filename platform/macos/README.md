> New Society storage is one schema-1 `.societycontainer` directory package.
> FSKit mounts its existing `Files/` section without copying or preallocating data.
> Legacy APFS disk images are not automatically opened or restored.

# macOS Society

The native host accepts only `.societycontainer` sources with the Society UUID
manifest and nine native section directories. Ordinary directories, nested
containers, Finder replicas, and sources containing `Info.plist`, `bands`, or
`.society-disk.plist` disk-image markers are rejected before registration.
The FSKit resource probe also requires the package extension and refuses those
legacy markers and redirected manifests.

On macOS 27, the signed host uses Apple's public
[`FSClient.mountSingleVolume`](https://developer.apple.com/documentation/fskit/fsclient/mountsinglevolume%28resource%3Abundleid%3Aoptions%3Acompletionhandler%3A%29)
with the `com.apple.developer.fskit.mount` capability and a security-scoped
resource URL. macOS creates and owns the mount point under `/Volumes/`.
Society never creates or removes an external mount directory. Stored data,
application state, logs and temporary files stay inside the selected package;
the OS volume is a direct projection of `Files/` and does not copy that data.

The macOS 26 command-line path prepares only an empty
`<container>/.society-runtime/mounts/<uuid>/Society` backing directory. It rejects
redirected ancestors, foreign mounted ancestors and nonempty directories.
That older path requires an ownership-enabled host filesystem and can be
limited by OS file-mount policy. macOS 27 does not fall back to it when its
public API or provisioning fails.

Reconnect examines actual `societyfs` mount records and the filesystem's volume
UUID. A renamed source renews its security-scoped resource through public unmount and
mount while retaining the same UUID; a different still-existing
source inode with the same UUID, or duplicate mounted UUIDs, is rejected.
Discovery creates no files. Unmount uses public Disk Arbitration and targets
only the matching filesystem source and UUID. OS authorization errors leave the
volume and its source intact and are reported to the caller.

`DirectoryMountTests.swift` checks legacy package preparation, UUID discovery,
rename reconnect and collision rejection. `tests/native_directory_mount.py`
checks the actual device, OS-managed macOS 27 mount point, Files-only CRUD,
rename reconnect and unchanged manifest UUID. It retains its backing fixture
when unmount fails. The standard test suite does not register or activate an OS
extension. File Provider materialization, extension registration, access grants
and the `/Volumes` namespace remain under macOS control.

`LocalDriveStore` protects `.society-write-*` copies with the same zero-byte
`.iisacc-temporary.lock` shared lease used by the C++ SDK. The lease begins before
copying and ends after publication or removal, so periodic maintenance skips
active writes. A redirected or nonregular lease is rejected without replacing
the destination. `LocalDriveStoreTests.swift` covers redirected-lease rejection.

The public File Provider and FSKit views hide `.iisacc-temporary.lock` and reject
direct item operations on this internal coordination metadata. An explicit
FSKit deletion of an otherwise empty folder may remove its idle zero-byte
sentinel while holding a nonblocking exclusive lease. An active writer returns
`EBUSY`, and other folder contents remain subject to ordinary `ENOTEMPTY`.
Background maintenance always preserves the sentinel. The real
`tests/native_directory_mount.py` regression covers listing, direct write
rejection, active-writer exclusion, and idle sentinel-only folder deletion.

FSKit reads and writes own a descriptor with the requested access mode for
the operation's lifetime. Reading a source-preloaded file before writing it,
or closing its cached item during I/O, preserves the user inode and does not
invalidate the operation descriptor. `native_io` exercises the read-only cache,
write reopening, cached-close overlap and descriptor cleanup. Attribute reads
and parent lookups likewise own their descriptor, including simultaneous
attribute requests after a cached close. Attribute snapshot creation and field population are serialized per item to preserve FSKit attribute sequence ordering. Concurrent cache misses share the
first published item for the same inode; reclaiming an older object does not
remove a later cached object.

Directory callbacks own independent `openat(".")` streams and close them on
completion, error and early return. Cookies count raw directory entries across
requests rather than reusing a `telldir` token from a different stream.
If requested enumeration attributes cannot be obtained, the callback returns
the actual error instead of packing missing attributes as a successful listing.
`native_directory` covers repeated and interleaved complete listings,
pagination and stream lifetime with 1,024 long filenames. The real mount
regression also checks concurrent listing, repeated read/write of preloaded
ordinary files, and read-only mutation rejection for metadata-name user data. Directory mutation
between paginated requests remains outside the unchanged verifier policy.

FSKit rejects new exact volume-root index names `.fseventsd` and
`.Spotlight-V100`, and new `.DS_Store` entries in every folder, before creating
source bytes.
Creation reports `EEXIST` for an existing matching entry and `EPERM` for a
missing matching entry. Renaming to a missing matching entry also reports
`EPERM`. Existing matching entries and descendants of matching folders remain
listed and available to lookup and reads. Mounted writes, removal, rename,
attribute mutation and preallocation report `EPERM`; replacement of a reserved
destination and creation inside a protected folder are also rejected. Users
can edit and move this data through the authoritative source `Files/` path.
Volume-management names in unprotected nested folders and other dotfiles
remain ordinary names.
The policy limits new Finder view, event-history and indexing metadata storage.
`.Trashes`, `.TemporaryItems`, and `.DocumentRevisions-V100` remain ordinary
names at the root as well, preserving user recovery, save transactions and
document versions. `FSKitMetadataTests.swift` checks the native policy without
activation, and the real mount regression checks the installed FSKit callbacks
and existing user-file fidelity. The manifest UUID and file UID/GID attributes
remain independent of these management names.

The SDK's name policy does not override macOS System Policy. On the observed
macOS 27 mount, the kernel denied the ordinary Python caller's
`file-write-create` at the volume-root `.DocumentRevisions-V100`, while an
ordinary APFS control directory allowed the same name and inner file writes.
This policy does not change document-version service permissions. The real
mount regression reports `EPERM` only for that new root `mkdir` as a separate
OS limitation after verifying that no source entry remains. Other errors,
including `.Trashes`/`.TemporaryItems` CRUD and writes inside a successfully
created revision folder, still fail the test. The native unit test continues
to require that `.DocumentRevisions-V100` is allowed by the SDK name policy.

On the same observed macOS 27 mount, source-preloaded user files at root
`.Spotlight-V100` and `.TemporaryItems/user.txt` remained listed or available
to lookup, but the kernel denied the ordinary Python caller's `file-read-data`
for those exact paths. The SDK adds no existing-item hiding or read-name
restriction. The real regression reports only `EPERM` from these two exact
existing root reads as separate OS limitations, asserting unchanged source
bytes and device/inode and explicitly leaving projection writes unverified
for those cases. New volume-root `.Trashes` and `.TemporaryItems` file creation,
write, readback and deletion passed before the preloaded-file check; these
remain strict checks with an explicit success record. Other ordinary existing
user items and writes after a successful read of the unreserved
`.TemporaryItems` path remain strict checks.
Reserved root and nested metadata-name entries instead require mutation
rejection and unchanged source bytes and device/inode. No recovery,
save, document-version or indexing-service permissions or native entitlements
are changed to bypass the observed OS protection.

In an earlier real fixture with creation rejection alone, a preloaded root
`.fseventsd` appeared in baseline and concurrent listings, then disappeared
from the source itself. No successful-delete caller record was obtained, so
this observation does not identify an OS service as the deleting actor.
Callbacks that allowed existing reserved-name mutations could not guarantee
source preservation. The read-only mounted protection now addresses that
boundary without deleting or hiding existing user data or limiting ordinary
user-document CRUD.

Apple's [replicated File Provider](https://developer.apple.com/documentation/fileprovider/replicated-file-provider-extension)provides Society drive to Finder and file dialogs. Minimum macOS 15. Foundation, FileProvider, UniformTypeIdentifiers, CryptoKit, CoreServices are frameworks included in the operating system, adding no FUSE, external drivers, or paid cloud services. The Swift compiler uses the Apple SDK, bundle creation uses Python 3, and restricted FSKit capabilities use Xcode automatic provisioning with the configured developer team. Apple framework usage conditions follow Apple SDK license.

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
build/iiSocietyContainerDriveTool create "/path/to"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
  -f "$PWD/build/native/Society.app"
pluginkit -a "$PWD/build/native/Society.app/Contents/PlugIns/SocietyContainerProvider.appex"
open -W -g -a "$PWD/build/native/Society.app" --args list
"build/native/Society.app/Contents/MacOS/SocietyContainerDrive" register "/path/to/Society.societycontainer"
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
