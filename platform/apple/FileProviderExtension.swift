import Foundation
import FileProvider
import UniformTypeIdentifiers
#if os(macOS)
import CoreServices
#endif
import OSLog

private func storeID(_ id: NSFileProviderItemIdentifier) -> String {
    id == .rootContainer ? "root" : id.rawValue
}

private func providerID(_ id: String) -> NSFileProviderItemIdentifier {
    id == "root" ? .rootContainer : .init(rawValue: id)
}

final class DriveItem: NSObject, NSFileProviderItem {
    let record: DriveRecord
    init(_ record: DriveRecord) { self.record = record }
    var itemIdentifier: NSFileProviderItemIdentifier { providerID(record.id) }
    var parentItemIdentifier: NSFileProviderItemIdentifier { providerID(record.parent) }
    var filename: String { record.name }
    var contentType: UTType { record.directory ? .folder : UTType(filenameExtension: (record.name as NSString).pathExtension) ?? .data }
    var documentSize: NSNumber? { NSNumber(value: record.directory ? 0 : record.size) }
    var creationDate: Date? { Date(timeIntervalSince1970: record.created) }
    var contentModificationDate: Date? { Date(timeIntervalSince1970: record.modified) }
    var itemVersion: NSFileProviderItemVersion {
        NSFileProviderItemVersion(contentVersion: record.contentVersion, metadataVersion: record.metadataVersion)
    }
    var capabilities: NSFileProviderItemCapabilities {
        var result: NSFileProviderItemCapabilities = [.allowsReading]
        if record.directory { result.insert(.allowsContentEnumerating) }
        if record.directory { result.insert(.allowsAddingSubItems) }
        if !record.protected {
            result.formUnion([.allowsRenaming, .allowsReparenting, .allowsDeleting])
            if !record.directory { result.insert(.allowsWriting) }
        }
        return result
    }
}

private func providerError(_ error: Error) -> Error {
    if let error = error as? DriveStoreError {
        switch error {
        case .missing: return NSFileProviderError(.noSuchItem)
        case .conflict: return CocoaError(.fileWriteFileExists)
        case .protectedItem: return CocoaError(.fileWriteNoPermission)
        case .staleVersion:
#if os(macOS)
            return NSFileProviderError(.versionNoLongerAvailable)
#else
            // The FileProvider version error is unavailable on iOS. Keep the
            // operation rejected without overwriting the newer source item.
            return CocoaError(.fileWriteFileExists, userInfo: [
                NSLocalizedDescriptionKey: "The item changed. Refresh it before retrying."
            ])
#endif
        case .invalid(let message): return NSError(domain: NSCocoaErrorDomain, code: CocoaError.fileReadCorruptFile.rawValue, userInfo: [NSLocalizedDescriptionKey: message])
        }
    }
    let nsError = error as NSError
    if nsError.domain == NSCocoaErrorDomain || nsError.domain == NSFileProviderErrorDomain { return error }
    return NSError(domain: NSCocoaErrorDomain, code: CocoaError.fileReadUnknown.rawValue, userInfo: [NSUnderlyingErrorKey: error])
}

final class DriveEnumerator: NSObject, NSFileProviderEnumerator {
    private let store: FilesDriveStore
    private let identifier: NSFileProviderItemIdentifier
    private var current: DriveSnapshot?
    init(store: FilesDriveStore, identifier: NSFileProviderItemIdentifier) {
        self.store = store; self.identifier = identifier
    }
    func invalidate() { current = nil }
    private func items(in snapshot: DriveSnapshot) -> [DriveRecord] {
        if identifier == .trashContainer { return [] }
        return snapshot.records.values.filter {
            identifier == .workingSet || ($0.id != "root" && $0.parent == storeID(identifier))
        }.sorted { $0.path < $1.path }
    }
    func enumerateItems(for observer: NSFileProviderEnumerationObserver, startingAt page: NSFileProviderPage) {
        do {
            let snapshot = try store.snapshot()
            current = snapshot
            observer.didEnumerate(items(in: snapshot).map(DriveItem.init))
            observer.finishEnumerating(upTo: nil)
        } catch { observer.finishEnumeratingWithError(providerError(error)) }
    }
    func enumerateChanges(for observer: NSFileProviderChangeObserver, from anchor: NSFileProviderSyncAnchor) {
        do {
            guard let anchorString = String(data: anchor.rawValue, encoding: .utf8), let previous = store.previousSnapshot(forChangesFrom: anchorString) else {
                throw NSFileProviderError(.syncAnchorExpired)
            }
            let snapshot = try store.snapshot()
            let oldItems = Dictionary(uniqueKeysWithValues: items(in: previous).map { ($0.id, $0) })
            let newItems = Dictionary(uniqueKeysWithValues: items(in: snapshot).map { ($0.id, $0) })
            // Include the root when its presentation changes so existing
            // domains receive the capability to create children after upgrade.
            observer.didUpdate(newItems.values.filter { oldItems[$0.id] != $0 }
                .sorted { $0.path < $1.path }.map(DriveItem.init))
            observer.didDeleteItems(withIdentifiers: oldItems.keys.filter { newItems[$0] == nil }.map(providerID))
            current = snapshot
            observer.finishEnumeratingChanges(upTo: NSFileProviderSyncAnchor(Data(snapshot.anchor.utf8)), moreComing: false)
        } catch { observer.finishEnumeratingWithError(providerError(error)) }
    }
    func currentSyncAnchor(completionHandler: @escaping (NSFileProviderSyncAnchor?) -> Void) {
        let snapshot = current ?? (try? store.snapshot())
        completionHandler(snapshot.map { NSFileProviderSyncAnchor(Data($0.anchor.utf8)) })
    }
}

#if os(macOS)
/// Watches Files recursively; File Provider owns its separate replica.
private final class SourceWatcher {
    private var stream: FSEventStreamRef?
    let changed: () -> Void
    init(paths: [String], changed: @escaping () -> Void) {
        self.changed = changed
        var context = FSEventStreamContext(version: 0, info: Unmanaged.passUnretained(self).toOpaque(), retain: nil, release: nil, copyDescription: nil)
        stream = FSEventStreamCreate(nil, { _, info, _, _, _, _ in
            guard let info = info else { return }
            Unmanaged<SourceWatcher>.fromOpaque(info).takeUnretainedValue().changed()
        }, &context, paths as CFArray, FSEventStreamEventId(kFSEventStreamEventIdSinceNow), 0.5,
        FSEventStreamCreateFlags(kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot))
        if let stream = stream {
            FSEventStreamSetDispatchQueue(stream, DispatchQueue.global(qos: .utility))
            FSEventStreamStart(stream)
        }
    }
    func stop() {
        if let stream = stream { FSEventStreamStop(stream); FSEventStreamInvalidate(stream); FSEventStreamRelease(stream) }
        stream = nil
    }
    deinit { stop() }
}

#else
/// iOS has no FSEvents. Coordinated source writes notify the presented Files tree.
private final class SourceWatcher: NSObject, NSFilePresenter {
    let presentedItemURL: URL?
    let presentedItemOperationQueue = OperationQueue()
    private let changed: () -> Void
    init(paths: [String], changed: @escaping () -> Void) {
        presentedItemURL = paths.first.map { URL(fileURLWithPath: $0, isDirectory: true) }
        self.changed = changed
        super.init()
        presentedItemOperationQueue.maxConcurrentOperationCount = 1
        NSFileCoordinator.addFilePresenter(self)
    }
    func presentedItemDidChange() { changed() }
    func presentedSubitemDidChange(at url: URL) { changed() }
    func presentedSubitemDidAppear(at url: URL) { changed() }
    func presentedSubitem(at oldURL: URL, didMoveTo newURL: URL) { changed() }
    func accommodatePresentedSubitemDeletion(at url: URL, completionHandler: @escaping (Error?) -> Void) {
        changed(); completionHandler(nil)
    }
    func stop() { NSFileCoordinator.removeFilePresenter(self) }
    deinit { stop() }
}
#endif

@objc(SocietyContainerFileProvider)
final class SocietyContainerFileProvider: NSObject, NSFileProviderReplicatedExtension {
    private var store: FilesDriveStore?
    private var initializationError: Error?
    private var scopedURL: URL?
    private let manager: NSFileProviderManager?
    private let queue = DispatchQueue(label: "com.iisacc.society.container.files")
    private var watcher: SourceWatcher?

    required init(domain: NSFileProviderDomain) {
        manager = NSFileProviderManager(for: domain)
        super.init()
        do {
            guard let catalogURL = Bundle.main.url(forResource: "Sections", withExtension: "json") else {
                throw DriveStoreError.invalid("The drive has no section catalog.")
            }
#if os(iOS)
            let location = try SharedDriveLocation.current()
            let root = try location.existingRoot()
            let driveID = domain.identifier.rawValue
#else
            guard let bookmark = domain.userInfo?["sourceBookmark"] as? Data,
                  let driveID = domain.userInfo?["societyDriveIdentifier"] as? String else {
                throw DriveStoreError.invalid("The drive has no source bookmark or section catalog.")
            }
            var stale = false
            let key = "source-bookmark-" + driveID
            let root: URL
            if let persistent = UserDefaults.standard.data(forKey: key),
               let saved = try? URL(resolvingBookmarkData: persistent, options: [.withSecurityScope, .withoutUI], relativeTo: nil, bookmarkDataIsStale: &stale),
               saved.startAccessingSecurityScopedResource() {
                root = saved
            } else {
                // Resolving the host's implicit bookmark grants temporary access to this process.
                root = try URL(resolvingBookmarkData: bookmark, options: [.withoutUI], relativeTo: nil, bookmarkDataIsStale: &stale)
            }
            scopedURL = root
            let persistent = try root.bookmarkData(options: [.withSecurityScope], includingResourceValuesForKeys: nil, relativeTo: nil)
            UserDefaults.standard.set(persistent, forKey: key)
#endif
            let catalog = try JSONDecoder().decode([DriveSection].self, from: Data(contentsOf: catalogURL))
            let source = try FilesDriveStore(root: root, catalog: catalog)
            guard source.manifest.identifier == driveID else {
                throw DriveStoreError.invalid("The source does not match this drive.")
            }
            store = source
            watcher = SourceWatcher(paths: [source.root.path]) { [weak self] in
                self?.signalChanges()
            }
        } catch {
            initializationError = error
            Logger(subsystem: "com.iisacc.society.container.drive", category: "source").error("Source initialization failed: \(String(describing: error), privacy: .public)")
        }
    }

    func invalidate() {
        watcher?.stop()
        watcher = nil
        scopedURL?.stopAccessingSecurityScopedResource()
        scopedURL = nil
    }
    private func source() throws -> FilesDriveStore {
        guard let store = store else { throw initializationError ?? NSFileProviderError(.serverUnreachable) }
        return store
    }
    private func signalChanges() { manager?.signalEnumerator(for: .workingSet) { _ in } }

    private func perform(_ work: @escaping () -> Void) -> Progress {
        let progress = Progress(totalUnitCount: 1)
        queue.async { work(); progress.completedUnitCount = 1 }
        return progress
    }

    func item(for identifier: NSFileProviderItemIdentifier, request: NSFileProviderRequest, completionHandler: @escaping (NSFileProviderItem?, Error?) -> Void) -> Progress {
        perform {
            do {
                let item = DriveItem(try self.source().item(storeID(identifier)))
                completionHandler(item, nil)
            } catch {
                Logger(subsystem: "com.iisacc.society.container.drive", category: "items").error("Item lookup failed: \(identifier.rawValue, privacy: .public), error: \(String(describing: error), privacy: .private)")
                completionHandler(nil, providerError(error))
            }
        }
    }
    func fetchContents(for identifier: NSFileProviderItemIdentifier, version: NSFileProviderItemVersion?, request: NSFileProviderRequest, completionHandler: @escaping (URL?, NSFileProviderItem?, Error?) -> Void) -> Progress {
        perform {
            do {
                let store = try self.source()
                let item = try store.item(storeID(identifier))
                if let version = version, version.contentVersion != item.contentVersion { throw DriveStoreError.staleVersion }
                guard let manager = self.manager else { throw NSFileProviderError(.providerNotFound) }
                let file = try store.copyContents(item.id, to: manager.temporaryDirectoryURL())
                // Detect a source edit during the copy before returning a mismatched version.
                guard try store.item(item.id).contentVersion == item.contentVersion else {
                    try? FileManager.default.removeItem(at: file)
                    throw DriveStoreError.staleVersion
                }
                completionHandler(file, DriveItem(item), nil)
            } catch { completionHandler(nil, nil, providerError(error)) }
        }
    }
    func createItem(basedOn template: NSFileProviderItem, fields: NSFileProviderItemFields, contents: URL?, options: NSFileProviderCreateItemOptions, request: NSFileProviderRequest, completionHandler: @escaping (NSFileProviderItem?, NSFileProviderItemFields, Bool, Error?) -> Void) -> Progress {
        perform {
            do {
                if template.contentType == .symbolicLink { throw DriveStoreError.invalid("Symbolic links are not supported by this drive.") }
                let item = try self.source().create(parent: storeID(template.parentItemIdentifier), name: template.filename,
                                                   directory: template.contentType == .folder, contents: contents)
                completionHandler(DriveItem(item), [], false, nil)
                self.signalChanges()
            } catch { completionHandler(nil, [], false, providerError(error)) }
        }
    }
    func modifyItem(_ item: NSFileProviderItem, baseVersion: NSFileProviderItemVersion, changedFields: NSFileProviderItemFields, contents: URL?, options: NSFileProviderModifyItemOptions, request: NSFileProviderRequest, completionHandler: @escaping (NSFileProviderItem?, NSFileProviderItemFields, Bool, Error?) -> Void) -> Progress {
        perform {
            do {
                let updated = try self.source().modify(storeID(item.itemIdentifier),
                    name: changedFields.contains(.filename) ? item.filename : nil,
                    parent: changedFields.contains(.parentItemIdentifier) ? storeID(item.parentItemIdentifier) : nil,
                    contents: changedFields.contains(.contents) ? contents : nil,
                    baseContentVersion: changedFields.contains(.contents) ? baseVersion.contentVersion : nil,
                    baseMetadataVersion: changedFields.contains(.filename) || changedFields.contains(.parentItemIdentifier) ? baseVersion.metadataVersion : nil)
                completionHandler(DriveItem(updated), [], false, nil)
                self.signalChanges()
            } catch { completionHandler(nil, [], false, providerError(error)) }
        }
    }
    func deleteItem(identifier: NSFileProviderItemIdentifier, baseVersion: NSFileProviderItemVersion, options: NSFileProviderDeleteItemOptions, request: NSFileProviderRequest, completionHandler: @escaping (Error?) -> Void) -> Progress {
        perform {
            do {
                let store = try self.source()
                let item = try store.item(storeID(identifier))
                if item.protected { throw NSError.fileProviderErrorForRejectedDeletion(of: DriveItem(item)) }
                if item.contentVersion != baseVersion.contentVersion {
                    throw NSError.fileProviderErrorForRejectedDeletion(of: DriveItem(item))
                }
                if item.directory && !options.contains(.recursive) {
                    if try !store.children(item.id).isEmpty { throw NSFileProviderError(.directoryNotEmpty) }
                }
                try store.remove(item.id, baseContentVersion: baseVersion.contentVersion, baseMetadataVersion: nil)
                completionHandler(nil)
                self.signalChanges()
            } catch DriveStoreError.missing { completionHandler(nil) }
            catch { completionHandler(providerError(error)) }
        }
    }
    func enumerator(for identifier: NSFileProviderItemIdentifier, request: NSFileProviderRequest) throws -> NSFileProviderEnumerator {
        do {
            let store = try source()
            if identifier != .workingSet && identifier != .trashContainer {
                _ = try store.item(storeID(identifier))
            }
            return DriveEnumerator(store: store, identifier: identifier)
        } catch { throw providerError(error) }
    }
}
