# File loading and app-local caches

`StorageDirectoryModel` and `DashboardFiles` expose filesystem-backed rows without
running scans or image decoding on the GUI thread. A host directory is authoritative:
it does not parse the whole synchronization catalogue to display its own files.
Replicas retain the catalogue's metadata-first and on-demand-download semantics.

## Listing cache

Directory snapshots live in the application's `CacheLocation/society/directories-v1`.
The key includes the absolute folder and every filter/sort option; the record also
binds to the current Society container ID. At most 128 snapshots of at most 16 MiB
each, with a combined 128 MiB budget, are retained. Writes are atomic, private to the
user, and skipped if unchanged.

A snapshot is provisional: it can paint names immediately, but `status == Ready`
still requires a fresh scan. `activate()` cannot open an item while Loading. A fresh
scan reconciles deletions, replacements, sizes, residency and ordering using row
diffs, preserving selection/scroll contracts. `snapshotRestored()` identifies that
first provisional publication. Missing/malformed caches fall back to a normal scan.

Native file/directory notifications are coalesced for 100 ms. Up to 64 watch handles
cover the current view and sync catalogue. A 10-second fallback covers watcher limits
and unavailable network-filesystem notifications, replacing the prior full scan every
second. The folder watch is installed before Ready; optional individual watches are
installed eight at a time between frames, never as a large blocking GUI operation.
Explicit refresh and download completion still reconcile immediately.
Large local directories distribute metadata inspection over the available CPU cores
(one worker per 256 entries up to the hardware thread count); small folders avoid
thread-start overhead. This does not create parallel full-file reads.

Dashboard Files, Published and Generation History scans execute concurrently using
C++ futures, preserving section boundaries and cancellation. Canonical-path boundary
checks no longer reopen/validate the entire drive separately for every entry.

## Hash and preview cache

Register `PreviewProvider` as `society-preview` in the QML engine. Render the model's
`fileThumbnailUrl` or Dashboard's `thumbnailSource`; keep `fileUrl`/`filePreviewUrl`
and Dashboard's existing `previewSource` for original/open and older consumer
contracts. Each cache URL binds the absolute canonical path, byte size,
high-resolution modification time and metadata-change time. Symlinks, missing files
and unsupported suffixes receive no thumbnail URL.

Only requested images are decoded. The GUI adapter uses a dedicated standard C++23
worker queue (available CPU count, capped at 8 desktop / 2 mobile to bound decoder
memory). Cancellation skips queued work. No global Qt or synchronization pool is
resized. Source files are never rewritten and placeholders are never downloaded just
to paint a tile; existing downloaded remote previews can use the same cache.

Cache misses generate a maximum 512-pixel lossless PNG and compute the source SHA-256.
The manifest and PNG are atomically written under `CacheLocation/society/previews-v1`
with owner-only permissions. Concurrent requests for the same revision are coalesced
by striped locks. Hits decode the small cached PNG without rehashing the original.
An edit during processing discards the result; missing/corrupt previews rebuild.
This hash is a rendering-cache digest, not a replacement for authenticated sync hashes.

The preview store is bounded to approximately 512 MiB, pruning oldest written entries
every 32 generated previews (there can be one batch of temporary overshoot). Sources
over 256 MiB or 32 megapixels are not decoded by this lightweight preview path. They
remain openable originals and use a placeholder in this path. Caches are rebuildable,
not a backup, not shared/synced, and removable by the OS. No credentials are stored.

`IISOCIETY_FILE_CACHE_DIRECTORY` can point to an absolute application/test cache root;
otherwise the OS application CacheLocation is used. Tests put their fixtures/caches
under `build/`. `iiSocietyContainerLoadingBenchmark [folder]` measures first rows and
fully reconciled Ready for three model instances; without a folder it creates a
2,000-file fixture in the current build directory. Cache and OS page-cache warmth
must be reported separately when comparing timings.

Regression targets: `preview_cache`, `shared_storage`, `dashboard`. They cover
persistent hits, SHA-256, overwrite/delete invalidation, concurrent requests, corrupted
previews, async completion/cancellation, snapshot reconciliation, and watcher refresh.
The filesystem-backed model-catalog readiness assertion allows up to 30 seconds so
external-volume contention does not turn a slow scan into a misleading five-second
test failure; it still checks asynchronous completion before validating the rows.
The Dashboard section-isolation regression repeatedly scans all three populated
sections, verifies row ownership and size labels, and checks that internal scan
fields do not escape into published model data. A heap-instrumented build may be
configured under `build/address-check` to investigate concurrency failures without
replacing the normal installed SDK.

Always rebuild both the SDK and its test consumers before running cached test
executables, especially after a GUI class layout changes:

```sh
cmake --build build --parallel 6 --target iiSocietyContainerPreviewCacheTests iiSocietyContainerDashboardTests iiSocietyContainerSharedStorageTests
ctest --test-dir build -R '^iiSocietyContainer.(preview_cache|dashboard|shared_storage)$' --output-on-failure
```
