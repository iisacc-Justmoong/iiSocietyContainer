# Persistent file presentation

## Required public file-information contract

Every readable, resident regular asset in a verified Society public section can
be inspected through iiFileProvider, irrespective of its extension and whether
it has been indexed or created by an iisacc app. An inspection finishes missing
presentation work on demand instead of depending on a background scan having
already reached that file. Original bytes retain their format. Unsupported or
over-budget media returns a truthful `information` preview; unreadable, missing,
redirected or concurrently changed files, or unavailable presentation storage,
return an explicit failure.

`FileTree::presentation(relativePath)` is the native C++23 interface. It returns
the provider's typed `PresentationRecord`, including file details, media
properties, preview bytes, source version and actual attribute embedding status.
Linking `iiSocietyContainer::FileTree` imports `iiFileProvider::Native` publicly
and does not import Qt. `entry`, `children` and `snapshot` retain metadata-only
traversal and never decode every listed original.

`SharedStorage::fileInformation(section, relativePath, error)` is the application
and host projection. It is blocking and should execute on a GUI's I/O worker.
The `iiFileProvider.FileInformation` schema-1 document includes:

| Field | Content |
| --- | --- |
| `container`, `path` | Pinned container identity and logical section-relative key; no native host path |
| `details` | Name, extension, detected MIME type, original byte size, nanosecond modification time, format-specific properties |
| `preview` | Kind, MIME type, dimensions, base64 bytes and SHA-256, using the provider's 512-pixel bounded representation |
| `sourceVersion`, `embedded` | Source-bound presentation version and actual native-attribute publication result |
| `classification.status` | `ready`, `absent` or `invalid`; never silently trusts malformed annotations |
| `classification.metadata` | Published title, description, keywords, 0–5 rating, color label, custom fields and collection keys when ready |
| `classification.collections` | Published stable collection keys and display names when ready |

The provider validates the classification companion without creating an index or
rewriting it. A missing classification does not prevent details/preview access;
an invalid classification does not turn into fabricated defaults. The original
presentation is revalidated after classification is read. The combined document
is limited to 3 MiB; preview and classification retain their stricter individual
limits. Reserved provider documents, directories, symlinks and path traversal
are excluded from asset inspection.

`StorageDirectoryModel` publishes `fileInformationProvider` and
`fileInformationKey` for file rows. These lightweight roles identify the common
provider and logical inspection key without decoding every directory entry.
iiSocietyHelper exposes `FileSystem::fileInformation` for selected local storage
and `Client::inspectHostFile` for asynchronous direct host access. A remote or
nonresident original can be inspected through that authenticated host interface
without materializing its payload. The host serves 256-KiB chunks, pins the
source version and whole information-document hash, and refuses mixed revisions.
See [the host protocol](../../iiSocietyHelper/docs/Sync/README.md).

## Intake and persistence

SocietyDrive consumers use iiFileProvider 0.7 to generate file details and
portable embedded previews. Opening a verified drive registers the public
sections, excluding Deleted, with the native intake monitor. Files copied into
the original drive by Finder, another file manager or another application are
observed after two stable scans. Source directory identity and the expected
manifest identity remain pinned; redirects and private/control trees are ignored.

The worker slices traversal and decoding so a large initial inventory does not
occupy the application event loop. The Qt preview cache reads the provider's
bounded PNG projection. Its thumbnail key normalizes native change-time over
the current process's own preview-attribute publication; adding a first embedded
preview therefore preserves other queued requests for the same content.

The native Apple Files adapter notifies after atomic content replacement. FSKit
notifies the Swift provider after writing/closing a descriptor and after rename
publication, including an empty newly created file. Provider preview documents
and temporary lease/control state are excluded from the public C++ FileTree,
Qt listing, dashboard and Swift Files projections.
Swift enumeration skips descendants only for a control directory. Encountering
a hidden companion or lease file must not skip the visible children of its
parent directory; the native Files regression checks this alongside stable IDs.
Directory projections use stable creation metadata because visible children have
their own versions. Writing private derived companions therefore does not create
spurious public anchors or evict historical snapshots.
Android's DocumentsProvider and internal listing use the owner's shared control
name rule through DriveStore path validation, including previously issued item
identifiers. Public hidden files remain valid. Private control directories do
not receive recursive native watchers. The host Java naming regression compiles
all Android bridge sources against an SDK jar and exercises these boundaries
without claiming device execution.

Original data forks remain byte-for-byte intact. Native attributes/streams carry
details and preview bytes where supported. The hidden
`.<filename>.iisacc-preview` companion retains the same portable asset projection
when a transfer discards extended attributes. Its source change token journals
the post-embedding native state. Classification `.iisacc.json` documents are
independent and remain under Catalog revision control.

Full representation, codec limits, lifecycle hooks and API examples are in
[iiFileProvider's presentation contract](../../iiFileProvider/docs/FILE_PRESENTATION.md).
The storage monitor is hosted by a process that owns the selected drive; a
Society window is not required when a storage consumer/daemon is running.
Opening the current deployed application alone does not prove that it contains
these source changes; build, package-consumer and target-runtime evidence are
reported separately.
