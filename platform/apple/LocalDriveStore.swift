import Foundation
import CryptoKit
import Darwin

struct DriveSection: Codable, Equatable {
    let id: String
    let name: String
    let path: String
}

struct DriveManifest: Codable {
    let type: String
    let schemaVersion: Int
    let identifier: String
    var displayName: String
    let sections: [DriveSection]
    var localIdentifier: String? = nil
    var replicaReady: Bool? = nil
    var filesLayoutVersion: Int? = 1
    var providerIdentifier: String { localIdentifier ?? identifier }
}

struct DriveRecord: Codable, Equatable {
    let id: String
    let parent: String
    let name: String
    let path: String
    let directory: Bool
    let identity: String
    let size: Int64
    let modified: Double
    let created: Double
    let generation: String

    var protected: Bool {
        id == "root" || id.hasPrefix("section:")
    }
    // Directories have no fetched file content. Their children carry their own
    // versions; child writes must not invalidate a queued directory operation.
    var contentVersion: Data { Data((directory ? "\(identity):directory" : "\(identity):\(size):\(generation)").utf8) }
    // Content writes change mtime, but must not invalidate an independent rename
    // or move queued by File Provider against unchanged naming metadata.
    var metadataVersion: Data { Data("\(parent):\(name)".utf8) }
}

struct DriveSnapshot: Codable {
    let anchor: String
    let records: [String: DriveRecord]
}

private struct DriveIndex: Codable {
    let schemaVersion: Int
    let driveIdentifier: String
    var snapshots: [DriveSnapshot]
}

enum DriveStoreError: Error, LocalizedError {
    case invalid(String), missing, conflict, protectedItem, staleVersion
    var errorDescription: String? {
        switch self {
        case .invalid(let message): return message
        case .missing: return "The drive item no longer exists."
        case .conflict: return "An item with that name already exists."
        case .protectedItem: return "The drive root and section roots are fixed."
        case .staleVersion: return "The source item has changed since it was last read."
        }
    }
}

/// The local source is separate from File Provider's system-managed replica.
/// All sections use identical filesystem operations.
final class LocalDriveStore {
    let root: URL
    let manifest: DriveManifest
    private let manager = FileManager.default
    private let lock = NSRecursiveLock()
    private let indexURL: URL
    private var index: DriveIndex
    private var accessDepth = 0

    init(root: URL, catalog: [DriveSection]) throws {
        self.root = root.resolvingSymlinksInPath().standardizedFileURL
        try Self.validateSourceLocation(self.root)
        let manifestURL = self.root.appendingPathComponent(".society-drive.json")
        guard try manifestURL.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink != true else {
            throw DriveStoreError.invalid("The manifest must not be a symbolic link.")
        }
        let data = try Data(contentsOf: manifestURL)
        guard data.count <= 65536 else { throw DriveStoreError.invalid("Drive manifest is too large.") }
        var loadedManifest = try JSONDecoder().decode(DriveManifest.self, from: data)
        guard loadedManifest.type == "SocietyDrive", loadedManifest.schemaVersion == 1,
              let uuid = UUID(uuidString: loadedManifest.identifier), uuid != UUID(uuid: (0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)),
              ["Society", "Society Container"].contains(loadedManifest.displayName),
              loadedManifest.sections == catalog, catalog.count == 9 else {
            throw DriveStoreError.invalid("Invalid or unsupported Society drive manifest.")
        }
        // Normalize presentation without rewriting an existing source manifest.
        loadedManifest.displayName = "Society"
        if let local = loadedManifest.localIdentifier {
            guard UUID(uuidString: local) != nil, loadedManifest.replicaReady == true else {
                throw DriveStoreError.invalid("The initial Society mirror is not ready.")
            }
        }
        manifest = loadedManifest
        for section in catalog {
            try Self.validateName(section.path)
            let url = self.root.appendingPathComponent(section.path)
            let values = try url.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
            guard values.isDirectory == true, values.isSymbolicLink != true,
                  url.resolvingSymlinksInPath().path == url.path else {
                throw DriveStoreError.invalid("A drive section is missing or redirected: \(section.name)")
            }
        }
        indexURL = self.root.appendingPathComponent(".society-drive-provider.json")
        if manager.fileExists(atPath: indexURL.path) {
            guard try indexURL.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink != true else {
                throw DriveStoreError.invalid("The drive index must not be a symbolic link.")
            }
            index = try JSONDecoder().decode(DriveIndex.self, from: Data(contentsOf: indexURL))
            guard index.schemaVersion == 1, index.driveIdentifier == manifest.identifier || manifest.localIdentifier != nil else {
                throw DriveStoreError.invalid("Invalid or unsupported drive index.")
            }
        } else {
            index = DriveIndex(schemaVersion: 1, driveIdentifier: manifest.identifier, snapshots: [])
        }
        index = DriveIndex(schemaVersion: 1, driveIdentifier: manifest.identifier, snapshots: index.snapshots)
    }

    // The extension and the containing app can hold separate store instances.
    // Coordinate the complete transaction and reload persisted IDs before scanning.
    private func withAccess<T>(_ work: () throws -> T) throws -> T {
        lock.lock(); defer { lock.unlock() }
        if accessDepth > 0 { return try work() }
        try Self.validateSourceLocation(root)
        let currentURL = root.appendingPathComponent(".society-drive.json")
        guard try currentURL.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink != true else {
            throw DriveStoreError.invalid("The manifest must not be redirected.")
        }
        let current = try JSONDecoder().decode(DriveManifest.self, from: Data(contentsOf: currentURL))
        guard current.identifier == manifest.identifier, current.replicaReady != false else {
            throw DriveStoreError.invalid("The Society mirror changed; reopen this drive.")
        }
        var coordinationError: NSError?
        var result: Result<T, Error>?
        NSFileCoordinator().coordinate(writingItemAt: indexURL, options: .forMerging, error: &coordinationError) { _ in
            accessDepth += 1
            defer { accessDepth -= 1 }
            result = Result {
                if manager.fileExists(atPath: indexURL.path) {
                    guard try indexURL.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink != true else {
                        throw DriveStoreError.invalid("The drive index must not be a symbolic link.")
                    }
                    let latest = try JSONDecoder().decode(DriveIndex.self, from: Data(contentsOf: indexURL))
                    guard latest.schemaVersion == 1, latest.driveIdentifier == manifest.identifier || manifest.localIdentifier != nil else {
                        throw DriveStoreError.invalid("Invalid or unsupported drive index.")
                    }
                    index = DriveIndex(schemaVersion: 1, driveIdentifier: manifest.identifier, snapshots: latest.snapshots)
                }
                return try work()
            }
        }
        if let result = result { return try result.get() }
        throw coordinationError ?? CocoaError(.fileReadUnknown) as NSError
    }

    static func validateSourceLocation(_ root: URL, home: URL = URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true)) throws {
        let resolved = root.resolvingSymlinksInPath().standardizedFileURL
#if os(macOS)
        let cloud = home.appendingPathComponent("Library/CloudStorage").resolvingSymlinksInPath().standardizedFileURL
        guard !resolved.pathComponents.starts(with: cloud.pathComponents) else {
            throw DriveStoreError.invalid("Finder's CloudStorage drive is a public replica. Choose the original Society source folder.")
        }
#endif
        // Bookmark-backed URLs can retain a base URL when deleting components.
        // Walk a finite component array and create plain paths for ancestors.
        var components = resolved.pathComponents
        while components.count > 1 {
            components.removeLast()
            let ancestor = URL(fileURLWithPath: NSString.path(withComponents: components), isDirectory: true)
            let manifest = ancestor.appendingPathComponent(".society-drive.json", isDirectory: false)
            if FileManager.default.fileExists(atPath: manifest.path)
                || (try? manifest.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) == true {
                throw DriveStoreError.invalid("A Society source cannot be inside another container. Choose its original source folder.")
            }
        }
    }

    static func validateName(_ name: String) throws {
        guard !name.isEmpty, name != ".", name != "..", !name.contains("/"),
              !name.contains("\0"), name.utf8.count <= 255 else {
            throw DriveStoreError.invalid("Invalid file or folder name.")
        }
    }

    /// Foundation may enumerate an iOS app group through /private/var even
    /// when its resolved root uses /var. Compare canonical path components,
    /// rather than slicing an enumerated URL by the root string's length.
    static func relativePath(of child: URL, under root: URL) throws -> String {
        let base = root.resolvingSymlinksInPath().standardizedFileURL.pathComponents
        let components = child.resolvingSymlinksInPath().standardizedFileURL.pathComponents
        guard components.count > base.count, components.starts(with: base) else {
            throw DriveStoreError.invalid("The enumerated item is outside the source container.")
        }
        return components.dropFirst(base.count).joined(separator: "/")
    }

    private func sourceURL(_ record: DriveRecord) throws -> URL {
        guard record.id != "root" else { return root }
        let url = root.appendingPathComponent(record.path).standardizedFileURL
        let resolved = url.resolvingSymlinksInPath().standardizedFileURL
        guard resolved.path == url.path, resolved.path.hasPrefix(root.path + "/"),
              manifest.sections.contains(where: { record.path == $0.path || record.path.hasPrefix($0.path + "/") }) else {
            throw DriveStoreError.invalid("The item leaves or redirects the drive section.")
        }
        return url
    }

    func snapshot() throws -> DriveSnapshot {
        return try withAccess {
            let previous = index.snapshots.last?.records ?? [:]
            var byPath: [String: String] = [:]
            for (id, record) in previous {
                guard id == record.id, byPath.updateValue(id, forKey: record.path) == nil else {
                    throw DriveStoreError.invalid("The drive index has duplicate or invalid records.")
                }
            }
            let byIdentity = Dictionary(grouping: previous.values, by: { $0.identity })
            var records: [String: DriveRecord] = [:]
            var candidates: [DriveRecord] = []

            func record(_ url: URL, path: String, parent: String, fixedID: String? = nil) throws -> DriveRecord {
                let values = try url.resourceValues(forKeys: [.isDirectoryKey, .isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey, .contentModificationDateKey, .creationDateKey])
                guard values.isSymbolicLink != true, values.isDirectory == true || values.isRegularFile == true else {
                    throw DriveStoreError.invalid("Only native files and directories are supported.")
                }
                var attributes = stat()
                guard url.withUnsafeFileSystemRepresentation({ lstat($0!, &attributes) }) == 0 else {
                    throw CocoaError(.fileReadNoSuchFile)
                }
                let identity = "\(attributes.st_dev):\(attributes.st_ino):\(attributes.st_birthtimespec.tv_sec):\(attributes.st_birthtimespec.tv_nsec)"
                return DriveRecord(id: fixedID ?? "", parent: parent, name: path.isEmpty ? manifest.displayName : url.lastPathComponent,
                                   path: path, directory: values.isDirectory == true, identity: identity,
                                   size: Int64(values.fileSize ?? 0), modified: values.contentModificationDate?.timeIntervalSince1970 ?? 0,
                                   created: values.creationDate?.timeIntervalSince1970 ?? 0,
                                   generation: "\(attributes.st_mtimespec.tv_sec):\(attributes.st_mtimespec.tv_nsec)")
            }

            let rootRecord = try record(root, path: "", parent: "root", fixedID: "root")
            // Provider metadata must not change the root's version on every index write.
            candidates.append(DriveRecord(id: "root", parent: "", name: manifest.displayName, path: "", directory: true,
                                          identity: rootRecord.identity, size: 0, modified: rootRecord.created, created: rootRecord.created, generation: "root"))
            for section in manifest.sections {
                let url = root.appendingPathComponent(section.path)
                guard url.resolvingSymlinksInPath().path == url.path else {
                    throw DriveStoreError.invalid("A drive section has been redirected.")
                }
                let sectionRecord = try record(url, path: section.path, parent: "", fixedID: "section:" + section.id)
                guard sectionRecord.directory else { throw DriveStoreError.invalid("A drive section is not a directory.") }
                candidates.append(sectionRecord)
                var traversalError: Error?
                guard let enumerator = manager.enumerator(at: url, includingPropertiesForKeys: [.isSymbolicLinkKey, .isDirectoryKey], options: [], errorHandler: { _, error in traversalError = error; return false }) else {
                    throw DriveStoreError.invalid("The section cannot be enumerated.")
                }
                for case let child as URL in enumerator {
                    let values = try child.resourceValues(forKeys: [.isSymbolicLinkKey, .isDirectoryKey, .isRegularFileKey])
                    if values.isSymbolicLink == true || (values.isDirectory != true && values.isRegularFile != true) {
                        enumerator.skipDescendants(); continue
                    }
                    let relative = try Self.relativePath(of: child, under: root)
                    let parentPath = (relative as NSString).deletingLastPathComponent
                    candidates.append(try record(child, path: relative, parent: parentPath))
                }
                if let error = traversalError { throw error }
            }
            // Reserve IDs for every surviving inode before considering path replacements.
            // Otherwise a new file at an old path can steal the ID of a moved file.
            let survivingIdentities = Set(candidates.map { $0.identity })
            var parentIDs = ["": "root"]
            for candidate in candidates {
                let samePath = byPath[candidate.path].flatMap { previous[$0] }
                let sameIdentity = byIdentity[candidate.identity]?.first(where: { records[$0.id] == nil })?.id
                let previousID = samePath.flatMap { old -> String? in
                    records[old.id] == nil && !survivingIdentities.contains(old.identity) ? old.id : nil
                }
                let sameItem = samePath.flatMap { old -> String? in
                    old.identity == candidate.identity && records[old.id] == nil ? old.id : nil
                }
                let id = candidate.id.isEmpty ? sameItem ?? sameIdentity ?? previousID ?? UUID().uuidString.lowercased() : candidate.id
                guard let parent = parentIDs[candidate.parent] else {
                    throw DriveStoreError.invalid("The source traversal returned an entry before its parent.")
                }
                records[id] = DriveRecord(id: id, parent: parent, name: candidate.name, path: candidate.path,
                                           directory: candidate.directory, identity: candidate.identity, size: candidate.size,
                                           modified: candidate.modified, created: candidate.created, generation: candidate.generation)
                if candidate.directory { parentIDs[candidate.path] = id }
            }
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.sortedKeys]
            let encoded = try encoder.encode(records)
            let anchor = SHA256.hash(data: encoded).map { String(format: "%02x", $0) }.joined()
            let result = DriveSnapshot(anchor: anchor, records: records)
            if index.snapshots.last?.anchor != anchor {
                index.snapshots.append(result)
                index.snapshots = Array(index.snapshots.suffix(8))
                try encoder.encode(index).write(to: indexURL, options: .atomic)
            }
            return result

        }
    }

    func snapshot(at anchor: String) -> DriveSnapshot? {
        try? withAccess { index.snapshots.first(where: { $0.anchor == anchor }) }
    }

    func item(_ identifier: String) throws -> DriveRecord {
        guard let item = try snapshot().records[identifier] else { throw DriveStoreError.missing }
        return item
    }

    func children(_ parent: String) throws -> [DriveRecord] {
        let current = try snapshot()
        guard current.records[parent]?.directory == true else { throw DriveStoreError.missing }
        return current.records.values.filter { $0.parent == parent && $0.id != "root" }.sorted { $0.name < $1.name }
    }

    func copyContents(_ identifier: String, to directory: URL) throws -> URL {
        return try withAccess {
            let item = try self.item(identifier)
            guard !item.directory else { throw DriveStoreError.invalid("A directory has no file content.") }
            let destination = directory.appendingPathComponent(UUID().uuidString)
            try manager.copyItem(at: sourceURL(item), to: destination)
            return destination

        }
    }

    func create(parent: String, name: String, directory: Bool, contents: URL?) throws -> DriveRecord {
        return try withAccess {
            try Self.validateName(name)
            let parentItem = try item(parent)
            guard parentItem.directory, parent != "root" else { throw DriveStoreError.protectedItem }
            let target = try sourceURL(parentItem).appendingPathComponent(name)
            guard !existsIncludingSymlink(target) else { throw DriveStoreError.conflict }
            if directory {
                try manager.createDirectory(at: target, withIntermediateDirectories: false)
            } else if let contents = contents {
                try manager.copyItem(at: contents, to: target)
            } else {
                guard manager.createFile(atPath: target.path, contents: Data()) else { throw CocoaError(.fileWriteUnknown) }
            }
            guard let created = try snapshot().records.values.first(where: { root.appendingPathComponent($0.path).path == target.path }) else { throw DriveStoreError.missing }
            return created

        }
    }

    func modify(_ identifier: String, name: String?, parent: String?, contents: URL?, baseContentVersion: Data?, baseMetadataVersion: Data?) throws -> DriveRecord {
        return try withAccess {
            let original = try item(identifier)
            if original.protected {
                // File Provider also reports incidental directory metadata changes after
                // editing children. Acknowledge those without changing the fixed layout.
                guard (name == nil || name == original.name), (parent == nil || parent == original.parent), contents == nil else {
                    throw DriveStoreError.protectedItem
                }
                return original
            }
            if contents != nil && original.directory { throw DriveStoreError.invalid("Cannot replace a directory with file content.") }
            if let version = baseContentVersion, version != original.contentVersion { throw DriveStoreError.staleVersion }
            if let version = baseMetadataVersion, version != original.metadataVersion { throw DriveStoreError.staleVersion }
            let nextName = name ?? original.name
            try Self.validateName(nextName)
            let nextParent = try item(parent ?? original.parent)
            guard nextParent.directory, nextParent.id != "root" else { throw DriveStoreError.protectedItem }
            let oldURL = try sourceURL(original)
            let nextURL = try sourceURL(nextParent).appendingPathComponent(nextName)
            guard nextURL.path != oldURL.path, !existsIncludingSymlink(nextURL) else {
                if nextURL.path != oldURL.path { throw DriveStoreError.conflict }
                if let contents = contents {
                    try replaceContents(of: oldURL, with: contents)
                }
                return try item(identifier)
            }
            guard !nextURL.path.hasPrefix(oldURL.path + "/") else { throw DriveStoreError.invalid("Cannot move a folder into itself.") }
            try manager.moveItem(at: oldURL, to: nextURL)
            // Persist the new path before an atomic content replacement changes its inode.
            _ = try snapshot()
            if let contents = contents {
                try replaceContents(of: nextURL, with: contents)
            }
            return try item(identifier)

        }
    }

    func remove(_ identifier: String, baseContentVersion: Data?, baseMetadataVersion: Data?) throws {
        return try withAccess {
            let item = try self.item(identifier)
            guard !item.protected else { throw DriveStoreError.protectedItem }
            if let version = baseContentVersion, version != item.contentVersion { throw DriveStoreError.staleVersion }
            if let version = baseMetadataVersion, version != item.metadataVersion { throw DriveStoreError.staleVersion }
            try manager.removeItem(at: sourceURL(item))
            _ = try snapshot()

        }
    }

    private func existsIncludingSymlink(_ url: URL) -> Bool {
        var info = stat()
        return url.withUnsafeFileSystemRepresentation { lstat($0!, &info) == 0 }
    }

    private func replaceContents(of destination: URL, with source: URL) throws {
        // Copy on disk, then rename atomically; large files are never loaded into memory.
        let temporary = root.appendingPathComponent(".society-write-" + UUID().uuidString)
        try manager.copyItem(at: source, to: temporary)
        defer { try? manager.removeItem(at: temporary) }
        let result = temporary.withUnsafeFileSystemRepresentation { temporaryPath in
            destination.withUnsafeFileSystemRepresentation { destinationPath in
                Darwin.rename(temporaryPath!, destinationPath!)
            }
        }
        if result != 0 { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
    }
}
