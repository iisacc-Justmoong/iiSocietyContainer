import Foundation

/// File Provider's public view. Society continues to use the complete container.
/// Every item operation enforces this boundary, including requests using old IDs.
final class FilesDriveStore {
    let root: URL
    var manifest: DriveManifest { source.manifest }
    private let source: LocalDriveStore
    private let section: DriveSection
    private let anchorPrefix = "files-v5:"

    init(root: URL, catalog: [DriveSection]) throws {
        source = try LocalDriveStore(root: root, catalog: catalog)
        guard let files = source.manifest.sections.first(where: { $0.id == "files" }) else {
            throw DriveStoreError.invalid("The drive has no Files section.")
        }
        section = files
        self.root = source.root.appendingPathComponent(files.path, isDirectory: true)
    }

    private var sectionID: String { "section:" + section.id }

    private func publicRecord(_ record: DriveRecord) -> DriveRecord? {
        if record.id == sectionID && record.path == section.path {
            // Child writes must not change the root's version or its fixed name.
            return DriveRecord(id: "root", parent: "root", name: manifest.displayName, path: "", directory: true,
                               identity: record.identity, size: 0, modified: record.created,
                               created: record.created, generation: "root")
        }
        let prefix = section.path + "/"
        guard record.path.hasPrefix(prefix), !record.id.hasPrefix("section:") else { return nil }
        return DriveRecord(id: record.id, parent: record.parent == sectionID ? "root" : record.parent,
                           name: record.name, path: String(record.path.dropFirst(prefix.count)),
                           directory: record.directory, identity: record.identity, size: record.size,
                           modified: record.modified, created: record.created, generation: record.generation)
    }

    private func project(_ snapshot: DriveSnapshot) -> DriveSnapshot {
        let records = snapshot.records.values.compactMap(publicRecord)
        return DriveSnapshot(anchor: anchorPrefix + snapshot.anchor,
                             records: Dictionary(uniqueKeysWithValues: records.map { ($0.id, $0) }))
    }

    func snapshot() throws -> DriveSnapshot { project(try source.snapshot()) }

    /// Legacy snapshots are used only to report removals during a 0.4 -> 0.5
    /// upgrade. They must never be used for enumeration, item lookup or fetching.
    func previousSnapshot(forChangesFrom anchor: String) -> DriveSnapshot? {
        if anchor.hasPrefix(anchorPrefix) {
            return source.snapshot(at: String(anchor.dropFirst(anchorPrefix.count))).map(project)
        }
        return source.snapshot(at: anchor)
    }

    private func sourceRecord(_ identifier: String) throws -> DriveRecord {
        // The internal Files section identifier is not a second public root.
        guard identifier == "root" || !identifier.hasPrefix("section:") else { throw DriveStoreError.missing }
        let record = try source.item(identifier == "root" ? sectionID : identifier)
        guard publicRecord(record) != nil else { throw DriveStoreError.missing }
        return record
    }

    func item(_ identifier: String) throws -> DriveRecord {
        guard let record = publicRecord(try sourceRecord(identifier)) else { throw DriveStoreError.missing }
        return record
    }

    func children(_ parent: String) throws -> [DriveRecord] {
        let current = try snapshot()
        guard current.records[parent]?.directory == true else { throw DriveStoreError.missing }
        return current.records.values.filter { $0.parent == parent && $0.id != "root" }.sorted { $0.name < $1.name }
    }

    func copyContents(_ identifier: String, to directory: URL) throws -> URL {
        let record = try sourceRecord(identifier)
        return try source.copyContents(record.id, to: directory)
    }

    func create(parent: String, name: String, directory: Bool, contents: URL?) throws -> DriveRecord {
        let parentRecord = try sourceRecord(parent)
        let created = try source.create(parent: parentRecord.id, name: name, directory: directory, contents: contents)
        guard let result = publicRecord(created) else { throw DriveStoreError.missing }
        return result
    }

    func modify(_ identifier: String, name: String?, parent: String?, contents: URL?, baseContentVersion: Data?, baseMetadataVersion: Data?) throws -> DriveRecord {
        let original = try sourceRecord(identifier)
        guard let visible = publicRecord(original) else { throw DriveStoreError.missing }
        if identifier == "root" {
            guard (name == nil || name == visible.name), (parent == nil || parent == "root"), contents == nil else {
                throw DriveStoreError.protectedItem
            }
            return visible
        }
        try validateVersions(visible, content: baseContentVersion, metadata: baseMetadataVersion)
        let nextParent = try sourceRecord(parent ?? visible.parent)
        let updated = try source.modify(original.id, name: name, parent: nextParent.id, contents: contents,
                                        baseContentVersion: baseContentVersion,
                                        baseMetadataVersion: baseMetadataVersion == nil ? nil : original.metadataVersion)
        guard let result = publicRecord(updated) else { throw DriveStoreError.missing }
        return result
    }

    func remove(_ identifier: String, baseContentVersion: Data?, baseMetadataVersion: Data?) throws {
        let original = try sourceRecord(identifier)
        guard identifier != "root", let visible = publicRecord(original) else { throw DriveStoreError.protectedItem }
        try validateVersions(visible, content: baseContentVersion, metadata: baseMetadataVersion)
        try source.remove(original.id, baseContentVersion: baseContentVersion,
                          baseMetadataVersion: baseMetadataVersion == nil ? nil : original.metadataVersion)
    }

    private func validateVersions(_ item: DriveRecord, content: Data?, metadata: Data?) throws {
        if let version = content, version != item.contentVersion { throw DriveStoreError.staleVersion }
        if let version = metadata, version != item.metadataVersion { throw DriveStoreError.staleVersion }
    }
}
