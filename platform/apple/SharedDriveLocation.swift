import Foundation

/// iOS source storage belongs to the app group, outside File Provider's replica
/// and outside the containing app's publicly shareable Documents directory.
struct SharedDriveLocation {
    let group: URL
    private let components = ["Library", "Application Support", "Society"]

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
        var current = group
        for component in components {
            current.appendPathComponent(component, isDirectory: true)
            let values: URLResourceValues
            do {
                values = try current.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
            } catch let error as NSError where error.domain == NSCocoaErrorDomain
                && error.code == CocoaError.fileReadNoSuchFile.rawValue && create {
                try manager.createDirectory(at: current, withIntermediateDirectories: false)
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
