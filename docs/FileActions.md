# File operations

`FileOperations` is the blocking, drive-bound filesystem API. Call it on a worker thread.
`FileActions`, in the optional `iiSocietyContainer::Gui` target, is its asynchronous
Qt GUI adapter. Consumers request the `Gui` CMake component; Core-only consumers
keep their existing Core link dependencies. `IISOCIETYCONTAINER_BUILD_GUI=OFF`
omits clipboard and sharing support.

Supported operations are clipboard file-URL copy, path copy, paste, duplicate,
rename, move to `Deleted`, and permanent removal from `Deleted`. macOS sharing
uses `NSSharingServicePicker`; no recipient is selected or message sent by the SDK.
Other platforms advertise `canShare=false` until a native adapter is supplied.

Public section roots and the three fixed Files directories cannot be renamed,
deleted, or replaced. Destinations cannot cross the selected drive or follow
symlinks/junctions. Paste may read explicitly selected ordinary external files.
Existing destination files are never overwritten; duplicate/paste/trash choose
an unused name. Rename reports a collision. Permanent removal is accepted only
under `Deleted`; the GUI obtains an explicit confirmation for that operation.

Copies use a hidden staging path and appear only after completion. APFS clones
are used when available, with ordinary copying on other filesystems. A changed
source or copy failure discards the staged output. Copy publication and ordinary
rename share the replication operation lock.

Delete bypasses materialization, residency checks, hashes, model/package inspection,
and the replication lock. Trash uses a native no-replace rename into `Deleted`;
it never falls back to copying bytes across filesystems. Permanent removal uses
ordinary filesystem unlink/recursive directory removal, only inside `Deleted`.
Directories may contain any number of files, so permanent removal costs directory
traversal and unlink operations, not reading model contents. Partial local packages
can be moved and removed as they stand. A remote-only item fails promptly with a
host-deletion message; neither deletion action queues a download. Existing sync
scans publish resulting changes later, independently of action completion.

The local authority is identified by its validated `.society-sync/primary.json`
descriptor. Its model and directory views honor actual path existence immediately,
so an old catalog cannot resurrect a deleted row. A stale tombstone also cannot
hide a newly moved/restored file in its directory view. Replicas retain remote-only
rows when their cache is absent; cache eviction is not treated as deletion.

Opening a context menu does not request content. Actions that use an original
materialize only the selected file or package through `StorageDirectoryModel`.
Currently a remote-only item must finish materializing before a local
rename operation; rename does not move a partially downloaded directory. Errors
are visible and originals remain in place. No host-wide pull is started.

`file_operations` covers packages, collisions, recoverable trash, permanent
removal boundaries, protected roots, and path redirection. Sparse 8 GiB fixtures,
stale catalogs, an occupied replication lock, partial packages, and nested symlinks
verify deletion does not depend on content or follow links outside the selected tree.
`file_actions` covers asynchronous clipboard/paste/rename/trash, selected remote
materialization, and no-download deletion. `shared_storage` covers host listing
updates before reindexing and preservation of remote-only replica rows.
