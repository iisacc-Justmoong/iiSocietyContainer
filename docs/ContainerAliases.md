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
