/*
See the LICENSE.txt file for this sample’s licensing information.

Abstract:
The custom class that implements a simplified file system.
*/

import Foundation
import FSKit

extension Logger {
    static let societyfs = Logger(subsystem: "com.iisacc.society.filesystem", category: "default")
}

/// Returns the current `errno` value as a `POSIXError`.
var posixErrno: POSIXError {
    POSIXError(POSIXError.Code(rawValue: errno) ?? .EINVAL)
}

/// Returns the result of the given block, or throws an error if `errno` is nonzero.
/// - Parameter block: The block to execute.
func throwErrno<T: SignedInteger>(_ block: () throws -> T) throws -> T {
    let ret = try block()
    guard ret >= 0 else {
        guard errno != 0 else {
            Logger.societyfs.error("Call to block failed, and errno is not set")
            return ret
        }
        throw posixErrno
    }
    return ret
}

/// Society keeps one public volume name independently of its source directory.
/// - Parameter path: The path to use to generate a volume name.
func createVolumeNameFromPath(_ path: String) -> FSFileName {
    return FSFileName(string: "Society")
}

/// A file system that passes through all operations to the underlying file system from a given FSPathURLResource.
@objc
class SocietyFileSystem: FSUnaryFileSystem & FSUnaryFileSystemOperations {

    var resource: FSPathURLResource?

    public override init() {
        Logger.societyfs.debug("\(#function): init")
    }

    /// Performs an operation to load a resource.
    /// - Parameters:
    ///   - resource: The resource to load.
    ///   - options: The options to use when loading the resource.
    ///   - replyHandler: The handler to call when load operation is complete with the volume, and any error.
    public func loadResource(resource: FSResource, options: FSTaskOptions, replyHandler: @escaping (FSVolume?, (any Error)?) -> Void) {
        guard let urlResource = resource as? FSPathURLResource else {
            Logger.societyfs.debug("\(#function): Invalid resource type")
            return replyHandler(nil, POSIXError(.EINVAL))
        }

        /// Handle any options present.
        ///
        /// This Module doesn't make use of options for loading. The only option to handle
        /// is `-f`, and that is because this Module doesn't support formatting:
        ///   If the force option is present and the file system doesn't support
        ///   formatting, this method should reply with the POSIX error ENOTSUP.
        ///
        for opt in options.taskOptions {
            if opt.contains("-f") {
                return replyHandler(nil, POSIXError(.ENOTSUP))
            }
        }

        guard urlResource.url.startAccessingSecurityScopedResource() else {
            Logger.societyfs.error("\(#function): Can't start accessing security scoped resource")
            return replyHandler(nil, POSIXError(.EACCES))
        }

        self.resource = urlResource
        do {
            self.containerStatus = .ready
            let root = try societyFilesRoot(urlResource.url)
            return replyHandler(try SocietyFSVolume(rootPath: root.path), nil)
        } catch let error {
            urlResource.url.stopAccessingSecurityScopedResource()
            self.resource = nil
            return replyHandler(nil, error)
        }
    }

    ///  Performs an operation to unload a resource.
    /// - Parameters:
    ///   - resource: The resource to unload.
    ///   - options: The options to use when unloading the resource.
    ///   - replyHandler: The handler to call when unload is complete.
    public func unloadResource(resource: FSResource, options: FSTaskOptions, replyHandler reply: @escaping ((any Error)?) -> Void) {
        guard let urlResource = resource as? FSPathURLResource else {
            Logger.societyfs.error("\(#function): Can't cast resource")
            return reply(POSIXError(.EINVAL))
        }
        guard let loadedResource = self.resource else {
            Logger.societyfs.error("\(#function): No resource was loaded")
            return reply(POSIXError(.EINVAL))
        }
        guard loadedResource.url == urlResource.url else {
            Logger.societyfs.error("\(#function): Invalid resource was given to unload")
            return reply(POSIXError(.EINVAL))
        }
        loadedResource.url.stopAccessingSecurityScopedResource()
        self.resource = nil
        return reply(nil)
    }

    /// Performs a probe operation on a resource.
    /// - Parameters:
    ///   - resource: The resource to probe.
    ///   - replyHandler: The handler to call when the probe operation is complete.
    public func probeResource(resource: FSResource, replyHandler: @escaping (FSProbeResult?, (any Error)?) -> Void) {
        guard let urlResource = resource as? FSPathURLResource else {
            Logger.societyfs.debug("\(#function): Can't cast resource")
            return replyHandler(nil, POSIXError(.ENODEV))
        }

        let name            = createVolumeNameFromPath(urlResource.url.path())
        guard urlResource.url.startAccessingSecurityScopedResource() else {
            return replyHandler(nil, POSIXError(.EACCES))
        }
        defer { urlResource.url.stopAccessingSecurityScopedResource() }
        let containerUUID: UUID
        do { containerUUID = try societyVolumeIdentifier(societyFilesRoot(urlResource.url).path) }
        catch { return replyHandler(nil, error) }
        let containerIdentifier = FSContainerIdentifier(uuid: containerUUID as UUID)
        let probeResult = FSProbeResult.usable(name: name.string ?? "", containerID: containerIdentifier)
        return replyHandler(probeResult, nil)
    }

}

// FSKit grants access only to the user-selected source. Expose the existing Files
// section while preserving the package layout and its manifest UUID.
func societyFilesRoot(_ source: URL) throws -> URL {
    let root = source.resolvingSymlinksInPath()
    guard root.pathExtension == "societycontainer" else { throw POSIXError(.EINVAL) }
    for marker in ["Info.plist", "bands", ".society-disk.plist"] {
        var attributes = stat()
        guard lstat(root.appendingPathComponent(marker).path, &attributes) != 0, errno == ENOENT else {
            throw POSIXError(.EINVAL)
        }
    }
    let manifestURL = root.appendingPathComponent(".society-drive.json")
    var manifestAttributes = stat()
    guard lstat(manifestURL.path, &manifestAttributes) == 0,
          manifestAttributes.st_mode & S_IFMT == S_IFREG,
          manifestAttributes.st_size <= 65536 else { throw POSIXError(.EINVAL) }
    let data = try Data(contentsOf: manifestURL)
    guard let manifest = try JSONSerialization.jsonObject(with: data) as? [String: Any],
          manifest["type"] as? String == "SocietyDrive",
          manifest["schemaVersion"] as? Int == 1,
          let id = manifest["identifier"] as? String, UUID(uuidString: id) != nil else {
        throw POSIXError(.EINVAL)
    }
    let files = root.appendingPathComponent("Files", isDirectory: true)
    var attributes = stat()
    guard lstat(files.path, &attributes) == 0, attributes.st_mode & S_IFMT == S_IFDIR else {
        throw POSIXError(.ENOTDIR)
    }
    return files
}

func societyVolumeIdentifier(_ filesPath: String) throws -> UUID {
    let root = URL(fileURLWithPath: filesPath).deletingLastPathComponent()
    let data = try Data(contentsOf: root.appendingPathComponent(".society-drive.json"))
    guard let manifest = try JSONSerialization.jsonObject(with: data) as? [String: Any],
          let identifier = manifest["identifier"] as? String, let uuid = UUID(uuidString: identifier) else {
        throw POSIXError(.EINVAL)
    }
    return uuid
}
