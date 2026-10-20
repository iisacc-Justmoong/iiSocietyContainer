# Application container mirrors and file Aliases

`iiSocietyContainer::Container` is a read-only QML binding object. It owns a
container ID, root address, availability, revision and Alias value snapshots.
Constructing it, reading an Alias or filtering recent files performs no filesystem
I/O. It does not mount storage, create placeholders, write payloads or infer
existence from a path. The storage bridge supplies its state with `synchronize()`.
`refresh()` requests a new snapshot from that bridge.

An Alias contains `id` (container ID and logical key), `containerId`, `key`,
`section`, `relativePath`, `name`, `exists`, `available`, `source`, `previewSource`,
`thumbnailSource`, size and version metadata. `exists` means the logical object
exists; `available` means its local payload can be read. A remote object may exist
without any local file or parent directory. Its source is then empty. Aliases are
metadata references, not filesystem symlinks or copied image payloads.

`iiSocietyHelper::FileSystem` owns a Container and synchronizes it from the pinned
Society drive and its catalog. Applications borrow this object and retain Alias
values. They request publication, canvas persistence and previews through Helper.
Source and replica identity remain separate from the application mirror. A
snapshot for another container cannot insert its Aliases into the current object.

The QObject properties are the GUI binding boundary. The C++23 FileTree and
Maintenance components retain their existing Qt-free interfaces.

`iiSocietyContainer.alias` verifies snapshot-only access, unavailable payloads,
removal by synchronization and rejection of Aliases from another container.

Repeated `alias(key)` reads use a per-container metadata index built when the
owner publishes a changed snapshot. It indexes the accepted, deduplicated and
sorted rows, and is replaced before `stateChanged` is delivered. Query changes
leave this unfiltered lookup intact; disconnection/replacement removes stale keys.
Returned Alias maps remain value snapshots and cannot mutate the owner's data.
Lookup never probes storage or publishes a notification.

The index is an owned private child of the QML-facing QObject adapter. Exported
`Container` headers, fields and object size remain unchanged so existing native
owners that embed it by value keep their binary layout. The alias regression
checks replacement, duplicate/foreign/path filtering, independent containers,
reentrant readers, caller-edited copies and a 10,000-row/2,000-query timing fixture.
The timing is metadata lookup only, not application frame rate or filesystem I/O.
