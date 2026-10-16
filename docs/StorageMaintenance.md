# Storage maintenance

`StorageMaintenance` maintains the ordinary Society directory using C++23 and
native filesystem operations. It does not depend on Qt and is exported as
`iiSocietyContainer::Maintenance`. The lower-level `iiFileProvider::Temporary`
component supplies cross-process temporary-file leases.

`SocietyDrive` registers its ordinary source directory when it opens. The first
scan runs asynchronously, then the same process worker scans every five minutes.
Repeated opens of the same root and UUID reuse the registration. All registered
roots share one worker, its registry is bounded to 128 roots, and normal process
shutdown joins the worker. Applications may explicitly call `runOnce()` when an
immediate synchronous report is needed.

```cpp
#include <StorageMaintenance.h>
using iiSocietyContainer::StorageMaintenance;
auto result = StorageMaintenance::runOnce(sourceDirectory, driveUuid);
StorageMaintenance::monitor(sourceDirectory, driveUuid);
```

Each scan requires an absolute native directory whose components are neither
symbolic links nor Windows reparse points. The bounded, complete JSON manifest
must identify `SocietyDrive`, `schemaVersion: 1`, and the expected nonzero UUID.
Duplicate identity keys, invalid JSON, oversized manifests, a changed UUID, and
legacy disk images are rejected. The native root identity and manifest are
checked again before deletion. Maintenance never mounts, restores, migrates, or
copies an old image.

## Temporary file ownership

The scan removes only known SDK temporary namespaces:

- Direct regular files with `.society-import-`, `.society-write-`,
  `.iifile-stage-`, `.society-install-`, or `.society-extract-` prefixes at the
  source root or beneath the nine known storage sections.
- `.society-objects/staging/snapshot-*` directories containing only the owned
  `payload` file, or empty snapshot directories. Unknown children preserve the
  complete directory.
- `.society-sync/transfers/<64 lowercase hex digits>.part` and
  `.society-sync/transfers/provider-<UUID>.part`.
- `.society-sync/uploads/<UUID>` regular files.
- Direct regular `.society-sync/capability-<UUID>` and
  `.society-sync/capability-<UUID>.link` files left by Windows storage capability
  probes. Other capability names and suffixes are preserved. Probe operations
  hold their shared lease on `.society-sync` for their complete lifetime.

Writers hold a shared `TemporaryLease` on the parent for the complete lifetime
of temporary state. Cleanup attempts an exclusive lease on that same parent
before rescanning and deleting. Busy parents are skipped. A resumable transfer
is protected while its owning replica retains its shared lease; an abandoned
unleased transfer is removed at the next scan with no age grace period.

The zero-byte `.iisacc-temporary.lock` sentinel remains stable after first use;
it must not be deleted or rotated, because different processes must lock the
same native file. An empty source with no cleanup candidates creates no lock,
maintenance directory, log, or state file.

Ordinary user `.tmp` and `.log` files, installed model weights, rejected imports,
committed objects, synchronization catalogs, and SQLite `-wal`/`-shm` files are
preserved. A matching symbolic link is also preserved, and its target is never
followed. This cleanup policy is not an object-history pruning policy.

## Finder metadata at owned roots

Maintenance also removes a direct regular `.DS_Store` only at the source root
and the `Models/` root when its first eight bytes are `00 00 00 01 42 75 64 31`
(version 1 followed by `Bud1`). It checks this signature before taking a parent
exclusive lease, rescans under that lease, and verifies the file handle again
before deletion. Text files, other headers, links, and redirected paths are
preserved. `.DS_Store` in `Files/`, the other sections, or nested model/package
directories is preserved. Operating-system event directories are outside this
policy. Finder may regenerate its root metadata; the next scan applies the same
bounded rule.

## Log retention

Only direct regular `*.log` files in the SDK-private directories
`.society-maintenance/logs`, `.society-sync/logs`, and `.society-logs` are subject
to retention. Each directory keeps one newest log by modification time. Ties
keep the lexicographically greatest filename. Other extensions and nested
directories are preserved. Log writers use the same shared parent lease, so an
active log writer defers retention for that parent.

## Standalone CLI

The installed Qt-free tool supports one scan or a long-running watcher:

```sh
iiSocietyContainerMaintenanceTool \
  --root /absolute/path/Society --identifier <drive-uuid> --once
iiSocietyContainerMaintenanceTool \
  --root /absolute/path/Society --identifier <drive-uuid> \
  --watch --interval-seconds 300
```

The watcher immediately scans once and writes one JSON report per interval to
standard output. SIGINT and SIGTERM stop it. An invalid root or scan error emits
the report and exits with status 1; malformed command-line arguments exit with
status 2. `--interval-seconds` accepts 1 through 86400 seconds.

Reports contain removed temporary-file count, removed log-file count, removed
Finder-cache file count (`cacheRemoved`), logical removed bytes, busy parent
count, and error strings. Snapshot directories do not
increment the file count. Logical removed bytes do not measure APFS shared or
compressed allocations.

```json
{"tempRemoved":3,"logsRemoved":2,"cacheRemoved":1,"bytesRemoved":2048,"skippedBusy":1,"errors":[]}
```

An installed native service can use `--watch` to perform the same policy when no
consumer application is running. This is separate from the process-local
monitor, and both use the same cross-process leases.

Society's installed background daemon opens the selected ordinary folder at
startup and refreshes that selection every five minutes, independently of
account sign-in or network ownership. This registers the SDK worker while the
desktop window is closed. Saved legacy images are rejected by `SharedStorage`
before registration. The daemon uses its existing operating-system service and
application permissions. A direct CLI diagnostic prints the JSON report to the
caller without creating persistent scheduler logs.

## Build and verification

Qt-free consumers discover the component and its standalone dependency:

```cmake
find_package(iiSocietyContainer CONFIG REQUIRED COMPONENTS Maintenance)
target_link_libraries(your_tool PRIVATE iiSocietyContainer::Maintenance)
```

The `iiSocietyContainer.maintenance` test verifies immediate cleanup, active
writer and transfer protection, deterministic log retention, ordinary files and
SQLite WAL preservation, strict identity checks, legacy-image rejection,
redirected path preservation, recognized root-only Finder caches and public
namesake preservation, a minimal empty source, and initial/subsequent
periodic scans. Platform compilation and installed consumption are separate
checks from a live operating-system service.
Society's `daemonMaintainsSelectedFolderWithoutAnAccount` integration test
verifies initial cleanup by the actual headless daemon process.

Maintenance requires a canonical `.societycontainer` directory package. A manifest in an ordinary directory does not grant cleanup authority. Legacy disk images remain rejected; neither the selected source nor an arbitrary ancestor can broaden the managed boundary.

Leased private model jobs use `.society-runtime/tmp/.society-model-merge-<UUID>` or `.society-model-package-<UUID>`. Unleased orphan workspaces are removed on the next scan; active jobs hold a shared parent lease. The standard iiLocalDiffusion merge staging name is `.iild-merge-<eight-character tempfile token>` and is checked exactly. Redirected entries preserve a whole orphan workspace. Managed `.society-runtime/logs/` follows the same latest-log retention rule.
