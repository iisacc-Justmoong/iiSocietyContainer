import Foundation
import Darwin
import FSKit
import DiskArbitration

// Stored data and runtime state belong to the package. macOS 27 owns its
// /Volumes mount namespace; macOS 26 uses an empty in-package backing directory.
enum DirectoryMount {
    static func location(source: URL, identifier: String) throws -> URL {
        guard UUID(uuidString: identifier) != nil else { throw DriveStoreError.invalid("Invalid Society drive identifier.") }
        let root = source.resolvingSymlinksInPath().standardizedFileURL
        try LocalDriveStore.validateSourceLocation(root)
        return root
            .appendingPathComponent(".society-runtime/mounts", isDirectory: true)
            .appendingPathComponent(identifier, isDirectory: true)
            .appendingPathComponent("Society", isDirectory: true)
    }

    static func prepareLocation(source: URL, identifier: String) throws -> URL {
        let root = source.resolvingSymlinksInPath().standardizedFileURL
        let point = try location(source: root, identifier: identifier)
        var rootInfo = stat()
        guard lstat(root.path, &rootInfo) == 0, rootInfo.st_mode & S_IFMT == S_IFDIR else {
            throw DriveStoreError.invalid("The Society container is unavailable.")
        }
        var current = root
        for component in [".society-runtime", "mounts", identifier, "Society"] {
            current.appendPathComponent(component, isDirectory: true)
            var info = stat()
            if lstat(current.path, &info) != 0 {
                guard errno == ENOENT else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
                try IisaccFileProvider.createDirectory(at: current, withIntermediateDirectories: false)
                guard lstat(current.path, &info) == 0 else {
                    throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
                }
            }
            guard info.st_mode & S_IFMT == S_IFDIR, info.st_dev == rootInfo.st_dev,
                  current.resolvingSymlinksInPath().standardizedFileURL.path == current.path else {
                throw DriveStoreError.invalid("The Society mount directory must remain inside its container.")
            }
        }
        guard try FileManager.default.contentsOfDirectory(atPath: point.path).isEmpty else {
            throw DriveStoreError.invalid("The Society mount directory contains files.")
        }
        return point
    }

    static func filesystem(_ path: URL) -> (type: String, source: String, point: String)? {
        var info = statfs()
        guard statfs(path.path, &info) == 0 else { return nil }
        func string<T>(_ value: inout T) -> String {
            withUnsafePointer(to: &value) { pointer in
                pointer.withMemoryRebound(to: CChar.self, capacity: MemoryLayout<T>.size) { String(cString: $0) }
            }
        }
        return (string(&info.f_fstypename), string(&info.f_mntfromname), string(&info.f_mntonname))
    }

    static func run(_ executable: String, _ arguments: [String]) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        let errors = Pipe()
        process.standardError = errors
        process.standardOutput = errors
        try process.run()
        process.waitUntilExit()
        let detail = String(data: errors.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        guard process.terminationStatus == 0 else {
            throw DriveStoreError.invalid("Society drive mount failed. Enable Society in System Settings > General > Login Items & Extensions > File System Extensions. " + detail.trimmingCharacters(in: .whitespacesAndNewlines))
        }
    }

    struct MountedVolume: Equatable {
        let type: String
        let source: String
        let point: String
        let identifier: String?
    }

    static func mountedVolumes() -> [MountedVolume] {
        let count = getfsstat(nil, 0, MNT_NOWAIT)
        guard count > 0 else { return [] }
        let capacity = Int(count) + 8
        let records = UnsafeMutablePointer<statfs>.allocate(capacity: capacity)
        defer { records.deallocate() }
        let actual = getfsstat(records, Int32(capacity * MemoryLayout<statfs>.stride), MNT_NOWAIT)
        guard actual > 0 else { return [] }
        func string<T>(_ value: inout T) -> String {
            withUnsafePointer(to: &value) { pointer in
                pointer.withMemoryRebound(to: CChar.self, capacity: MemoryLayout<T>.size) { String(cString: $0) }
            }
        }
        return (0..<min(Int(actual), capacity)).compactMap { index in
            var record = records[index]
            let type = string(&record.f_fstypename)
            guard type == "societyfs" else { return nil }
            let point = string(&record.f_mntonname)
            let values = try? URL(fileURLWithPath: point).resourceValues(forKeys: [.volumeUUIDStringKey])
            return MountedVolume(type: type, source: string(&record.f_mntfromname), point: point,
                                 identifier: values?.volumeUUIDString)
        }
    }

    // The filesystem's UUID remains authoritative after the selected package is
    // renamed. A copied UUID at a different still-existing inode is a collision.
    static func existingVolume(source: URL, identifier: String, volumes: [MountedVolume]) throws -> MountedVolume? {
        let root = source.resolvingSymlinksInPath().standardizedFileURL
        let matching = volumes.filter { volume in
            guard volume.type == "societyfs", let current = volume.identifier,
                  UUID(uuidString: current) == UUID(uuidString: identifier) else { return false }
            return true
        }
        guard matching.count <= 1 else { throw DriveStoreError.invalid("Multiple Society volumes have the same container UUID.") }
        guard let volume = matching.first else { return nil }
        guard let original = URL(string: volume.source), original.isFileURL else {
            throw DriveStoreError.invalid("The Society volume has an invalid source identity.")
        }
        var currentInfo = stat(), originalInfo = stat()
        if lstat(original.path, &originalInfo) == 0 {
            guard lstat(root.path, &currentInfo) == 0,
                  currentInfo.st_dev == originalInfo.st_dev, currentInfo.st_ino == originalInfo.st_ino else {
                throw DriveStoreError.invalid("This Society UUID is mounted from a different container.")
            }
        } else if errno != ENOENT {
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
        return volume
    }

    static func needsSourceReload(_ volume: MountedVolume, source: URL) -> Bool {
        guard let original = URL(string: volume.source), original.isFileURL,
              original.standardizedFileURL.path != source.standardizedFileURL.path else { return false }
        var info = stat()
        return lstat(original.path, &info) != 0 && errno == ENOENT
    }

    static func report(_ volume: MountedVolume, store: LocalDriveStore) -> [String: Any] {
        ["identifier": store.manifest.identifier, "sourcePath": store.root.path,
         "systemPath": volume.point, "filesystemType": volume.type, "isMounted": true, "enabled": true]
    }

    static func perform(_ command: String, source: URL, catalog: [DriveSection],
                        completion: @escaping ([String: Any]?, Error?) -> Void) {
        do {
            guard #available(macOS 26.0, *) else { throw DriveStoreError.invalid("Directory drive mounting requires macOS 26 or later.") }
            let store = try LocalDriveStore(root: source.resolvingSymlinksInPath(), catalog: catalog)
            let existing = try existingVolume(source: store.root, identifier: store.manifest.identifier, volumes: mountedVolumes())
            if command == "unmount" {
                guard let volume = existing else {
                    completion(["identifier": store.manifest.identifier, "isMounted": false, "enabled": false], nil)
                    return
                }
                // Disk Arbitration owns lifecycle of OS-created mount points.
                // The caller never removes a /Volumes directory itself.
                unmount(volume) { error in
                    if let error { completion(nil, error); return }
                    completion(["identifier": store.manifest.identifier, "isMounted": false, "enabled": false], nil)
                }
                return
            }
            if let existing {
                if needsSourceReload(existing, source: store.root) {
                    // A filesystem resource's path grant must be renewed after
                    // source rename; UUID discovery alone cannot update it.
                    unmount(existing) { error in
                        if let error { completion(nil, error); return }
                        perform(command, source: store.root, catalog: catalog, completion: completion)
                    }
                } else { completion(report(existing, store: store), nil) }
                return
            }
            let module = Bundle.main.bundleURL.appendingPathComponent("Contents/Extensions/SocietyDriveFS.appex")
            guard FileManager.default.fileExists(atPath: module.path) else {
                throw DriveStoreError.invalid("The signed Society filesystem module is not installed.")
            }
            if #available(macOS 27.0, *) {
                let bookmark = try store.root.bookmarkData(options: [.withSecurityScope], includingResourceValuesForKeys: nil, relativeTo: nil)
                var stale = false
                let scoped = try URL(resolvingBookmarkData: bookmark, options: [.withSecurityScope], relativeTo: nil, bookmarkDataIsStale: &stale)
                guard !stale, scoped.startAccessingSecurityScopedResource() else {
                    throw DriveStoreError.invalid("macOS did not grant access to the selected Society container.")
                }
                let resource = FSPathURLResource(url: scoped, writable: true)
                FSClient.shared.mountSingleVolume(resource: resource, bundleID: "com.iisacc.society.container.drive.fskit", options: []) { point, error in
                    defer { scoped.stopAccessingSecurityScopedResource() }
                    if let error { completion(nil, error); return }
                    do {
                        guard let point, let volume = try existingVolume(source: store.root, identifier: store.manifest.identifier, volumes: mountedVolumes()),
                              volume.point == point.path else {
                            throw DriveStoreError.invalid("Society did not become a mounted filesystem with its container UUID.")
                        }
                        completion(report(volume, store: store), nil)
                    } catch { completion(nil, error) }
                }
                return
            }
            let point = try location(source: store.root, identifier: store.manifest.identifier)
            if filesystem(point)?.point == point.path { throw DriveStoreError.invalid("Another filesystem occupies the Society mount point.") }
            _ = try prepareLocation(source: store.root, identifier: store.manifest.identifier)
            try run("/sbin/mount", ["-F", "-t", "society", store.root.path, point.path])
            guard let volume = try existingVolume(source: store.root, identifier: store.manifest.identifier, volumes: mountedVolumes()), volume.point == point.path else {
                throw DriveStoreError.invalid("Society did not become a mounted filesystem.")
            }
            completion(report(volume, store: store), nil)
        } catch { completion(nil, error) }
    }

    private final class UnmountRequest {
        let session: DASession
        let completion: (Error?) -> Void
        init(session: DASession, completion: @escaping (Error?) -> Void) {
            self.session = session; self.completion = completion
        }
    }

    static func unmount(_ volume: MountedVolume, completion: @escaping (Error?) -> Void) {
        let point = URL(fileURLWithPath: volume.point)
        guard let actual = filesystem(point), actual.type == volume.type, actual.source == volume.source, actual.point == volume.point,
              let session = DASessionCreate(kCFAllocatorDefault),
              let disk = DADiskCreateFromVolumePath(kCFAllocatorDefault, session, point as CFURL) else {
            completion(DriveStoreError.invalid("The selected Society volume is no longer mounted.")); return
        }
        DASessionScheduleWithRunLoop(session, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
        let context = Unmanaged.passRetained(UnmountRequest(session: session, completion: completion)).toOpaque()
        DADiskUnmount(disk, DADiskUnmountOptions(kDADiskUnmountOptionDefault), { _, dissenter, context in
            guard let context else { return }
            let request = Unmanaged<UnmountRequest>.fromOpaque(context).takeRetainedValue()
            DASessionUnscheduleFromRunLoop(request.session, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
            if let dissenter {
                let code = DADissenterGetStatus(dissenter)
                request.completion(NSError(domain: "SocietyDiskArbitration", code: Int(code), userInfo: [NSLocalizedDescriptionKey:
                    "macOS could not eject the Society drive (Disk Arbitration \(code)). Use the system's authenticated eject flow before disconnecting its container."]))
            } else { request.completion(nil) }
        }, context)
    }
}
