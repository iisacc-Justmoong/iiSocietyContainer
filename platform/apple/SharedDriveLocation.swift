import Foundation

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
        let manager = FileManager.default
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
}
