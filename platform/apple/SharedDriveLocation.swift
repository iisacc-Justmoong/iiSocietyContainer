import Foundation
import Darwin

/// iOS source storage belongs to the app group, outside File Provider's replica
/// and outside the containing app's publicly shareable Documents directory.
struct SharedDriveLocation {
    let group: URL
    private let components = ["Society.societycontainer"]

    init(group: URL) {
        self.group = group.resolvingSymlinksInPath().standardizedFileURL
    }

    static func current(bundle: Bundle = .main) throws -> SharedDriveLocation {
        guard let identifier = bundle.object(forInfoDictionaryKey: "SocietyAppGroup") as? String,
              !identifier.isEmpty,
              let group = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: identifier) else {
            throw DriveStoreError.invalid("Society and its File Provider need the same App Group entitlement.")
        }
        return SharedDriveLocation(group: group)
    }

    func prepareRoot() throws -> URL { try sourceRoot(create: true) }
    func existingRoot() throws -> URL { try sourceRoot(create: false) }

    func validate(root: URL) throws {
        guard try existingRoot().path == root.resolvingSymlinksInPath().standardizedFileURL.path else {
            throw DriveStoreError.invalid("On iOS, connect Society's shared app container to Files.")
        }
    }

    private func sourceRoot(create: Bool) throws -> URL {
        // The C++ bootstrapper uses the same advisory lock. Keep its inode in
        // place so simultaneous first launches cannot select different disks.
        let lock = IisaccFileProvider.open(group.appendingPathComponent(".iisacc-storage.guard").path, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard lock >= 0 else { throw DriveStoreError.invalid("The shared storage ownership lock is unavailable.") }
        defer { flock(lock, LOCK_UN); close(lock) }
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        while flock(lock, LOCK_EX | LOCK_NB) != 0 {
            guard errno == EWOULDBLOCK && ProcessInfo.processInfo.systemUptime < deadline else {
                throw DriveStoreError.invalid("Another application is preparing shared storage.")
            }
            usleep(10_000)
        }
        let manager = FileManager.default
        if let source = try registeredSource(adopt: create) { return source }
        let target = group.appendingPathComponent("Society.societycontainer", isDirectory: true)
        let previous = group.appendingPathComponent("Library/Application Support/Society", isDirectory: true)
        if create && !manager.fileExists(atPath: target.path) && manager.fileExists(atPath: previous.path) {
            guard previous.resolvingSymlinksInPath().standardizedFileURL.path == previous.path,
                  try previous.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey]).isDirectory == true else {
                throw DriveStoreError.invalid("The previous Society source is redirected.")
            }
            for marker in ["Info.plist", "bands", ".society-disk.plist"] {
                let url = previous.appendingPathComponent(marker)
                guard !manager.fileExists(atPath: url.path),
                      (try? url.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) != true else {
                    throw DriveStoreError.invalid("Legacy Society disk images are not restored.")
                }
            }
            let manifestURL = previous.appendingPathComponent(".society-drive.json")
            guard try manifestURL.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink != true else {
                throw DriveStoreError.invalid("The previous Society manifest is redirected.")
            }
            let data = try Data(contentsOf: manifestURL)
            guard data.count <= 65536 else { throw DriveStoreError.invalid("The previous Society manifest is too large.") }
            let manifest = try JSONDecoder().decode(DriveManifest.self, from: data)
            guard manifest.type == "SocietyDrive", manifest.schemaVersion == 1,
                  UUID(uuidString: manifest.identifier) != nil, manifest.sections.count == 9 else {
                throw DriveStoreError.invalid("The previous Society source is unsupported.")
            }
            for section in manifest.sections {
                try LocalDriveStore.validateName(section.path)
                let url = previous.appendingPathComponent(section.path)
                let values = try url.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
                guard values.isDirectory == true, values.isSymbolicLink != true else {
                    throw DriveStoreError.invalid("The previous Society source has an invalid section.")
                }
            }
            try manager.moveItem(at: previous, to: target)
        }
        var current = group
        for component in components {
            current.appendPathComponent(component, isDirectory: true)
            let values: URLResourceValues
            do {
                values = try current.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
            } catch let error as NSError where error.domain == NSCocoaErrorDomain
                && error.code == CocoaError.fileReadNoSuchFile.rawValue && create {
                try IisaccFileProvider.createDirectory(at: current, withIntermediateDirectories: false)
                values = try current.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
            }
            guard values.isDirectory == true, values.isSymbolicLink != true,
                  current.resolvingSymlinksInPath().path == current.path else {
                throw DriveStoreError.invalid("The shared Society source must not be redirected.")
            }
        }
#if os(iOS)
        if create {
            try manager.setAttributes([.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication],
                                      ofItemAtPath: current.path)
        }
#endif
        return current
    }

    // Relative registrations survive application reinstall and App Group path
    // spelling changes. Every existing component is checked before opening it.
    private func sharedPath(_ relative: String, directory: Bool, missingAllowed: Bool = false) throws -> URL {
        guard !relative.isEmpty, !relative.hasPrefix("/"), !relative.contains("\\"), !relative.contains(":"), !relative.contains("\0") else {
            throw DriveStoreError.invalid("Invalid shared storage path.")
        }
        let parts = relative.split(separator: "/", omittingEmptySubsequences: false)
        var current = group
        for (index, part) in parts.enumerated() {
            guard !part.isEmpty, part != ".", part != ".." else { throw DriveStoreError.invalid("Invalid shared storage path.") }
            current.appendPathComponent(String(part))
            var state = stat()
            if lstat(current.path, &state) != 0 {
                if errno == ENOENT && missingAllowed { continue }
                throw DriveStoreError.invalid("The shared storage source is unavailable.")
            }
            let expected = index == parts.count - 1 && !directory ? S_IFREG : S_IFDIR
            guard state.st_mode & S_IFMT == expected, current.resolvingSymlinksInPath().path == current.path else {
                throw DriveStoreError.invalid("The shared storage source is redirected or has conflicting contents.")
            }
        }
        return current
    }

    private func registeredSource(adopt: Bool) throws -> URL? {
        let registry = try sharedPath("Library/Application Support/iisacc/Society/storage.json", directory: false, missingAllowed: true)
        var state = stat()
        if lstat(registry.path, &state) != 0 {
            if errno == ENOENT { return nil }
            throw DriveStoreError.invalid("The shared storage registration is unavailable.")
        }
        guard state.st_size > 0, state.st_size <= 65536 else { throw DriveStoreError.invalid("The shared storage registration is too large.") }
        let original = try Data(contentsOf: registry)
        guard var record = try JSONSerialization.jsonObject(with: original) as? [String: Any],
              record["schemaVersion"] as? Int == 1, let relative = record["path"] as? String,
              let owner = record["ownerApplication"] as? String, LocalDriveStore.validApplicationIdentifier(owner),
              let manager = record["managingApplication"] as? String, LocalDriveStore.validApplicationIdentifier(manager),
              let storage = record["storageId"] as? String, UUID(uuidString: storage) != nil,
              let logical = record["containerId"] as? String, UUID(uuidString: logical) != nil,
              let kind = record["storageKind"] as? String, ["logical", "package"].contains(kind) else {
            throw DriveStoreError.invalid("The shared storage registration is invalid or unsupported.")
        }
        let root = try sharedPath(relative, directory: true)
        let manifestURL = root.appendingPathComponent(".society-drive.json")
        let manifestState = try manifestURL.resourceValues(forKeys: [.isSymbolicLinkKey, .fileSizeKey])
        guard manifestState.isSymbolicLink != true, (manifestState.fileSize ?? 65537) <= 65536 else {
            throw DriveStoreError.invalid("The registered drive manifest is invalid or redirected.")
        }
        let manifest = try JSONDecoder().decode(DriveManifest.self, from: Data(contentsOf: manifestURL))
        let declaredOwner = try LocalDriveStore.logicalOwner(at: root)
        let validKind = kind == "logical" ? declaredOwner == owner : root.pathExtension == "societycontainer"
        guard manifest.providerIdentifier == storage, validKind else {
            throw DriveStoreError.invalid("The registered disk identity or owner changed.")
        }
        // Validate all sections and readiness before changing ownership. The
        // platform bridge validates its canonical bundled catalog again on open.
        _ = try LocalDriveStore(root: root, catalog: manifest.sections)
        if adopt && manager != "com.iisacc.society" {
            var coordinationError: NSError?
            var result: Result<Void, Error>?
            NSFileCoordinator().coordinate(writingItemAt: registry, options: .forMerging, error: &coordinationError) { _ in
                result = Result {
                    guard try Data(contentsOf: registry) == original else { throw DriveStoreError.staleVersion }
                    record["managingApplication"] = "com.iisacc.society"
                    try IisaccFileProvider.write(JSONSerialization.data(withJSONObject: record, options: [.sortedKeys]), to: registry)
                }
            }
            if let result = result { try result.get() }
            else { throw coordinationError ?? CocoaError(.fileWriteUnknown) as NSError }
        }
        return root
    }
}
