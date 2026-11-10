import Foundation
import Darwin

// FSKit projects only the existing Files section; the package and its UUID
// remain authoritative and no copied filesystem is created.
func societyFilesRoot(_ source: URL) throws -> URL {
    let root = source.resolvingSymlinksInPath()
    // Match the host's source contract, including explicitly owned logical
    // disks; an ordinary folder is never promoted merely by containing Files.
    try LocalDriveStore.validateSourceLocation(root)
    let manifestURL = root.appendingPathComponent(".society-drive.json")
    var manifestAttributes = stat()
    guard lstat(manifestURL.path, &manifestAttributes) == 0,
          manifestAttributes.st_mode & S_IFMT == S_IFREG,
          manifestAttributes.st_size <= 65536 else { throw POSIXError(.EINVAL) }
    let data = try Data(contentsOf: manifestURL)
    guard let manifest = try JSONSerialization.jsonObject(with: data) as? [String: Any],
          manifest["type"] as? String == "SocietyDrive",
          manifest["schemaVersion"] as? Int == 1,
          let id = manifest["identifier"] as? String, let uuid = UUID(uuidString: id),
          uuid != UUID(uuid: (0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)) else {
        throw POSIXError(.EINVAL)
    }
    if let local = manifest["localIdentifier"] {
        guard let localID = local as? String, UUID(uuidString: localID) != nil,
              manifest["replicaReady"] as? Bool == true else { throw POSIXError(.EAGAIN) }
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
