import Foundation
import Darwin

private func check(_ condition: @autoclosure () -> Bool, _ message: String) throws {
    if !condition() { throw NSError(domain: "SocietyFSMetadataTests", code: 1,
                                   userInfo: [NSLocalizedDescriptionKey: message]) }
}

private func expectNewMetadataRejection(directory: Int32, name: String, atVolumeRoot: Bool) throws {
    do {
        _ = try societyExistingMetadataDestination(directory: directory, name: name, atVolumeRoot: atVolumeRoot)
        throw NSError(domain: "SocietyFSMetadataTests", code: 2,
                      userInfo: [NSLocalizedDescriptionKey: "A missing metadata destination must be rejected: " + name])
    } catch let error as POSIXError {
        try check(error.code == .EPERM, "New management entries must report EPERM")
    }
}

private func expectProtectedFile(_ file: URL, name: String, atVolumeRoot: Bool,
                                 insideProtectedDirectory: Bool) throws {
    let originalBytes = try Data(contentsOf: file)
    var original = stat()
    guard lstat(file.path, &original) == 0 else {
        throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
    }
    let blocked = societyMetadataMutationIsBlocked(name: name, atVolumeRoot: atVolumeRoot,
                                                    insideProtectedDirectory: insideProtectedDirectory)
    try check(blocked, "A protected existing entry must reject mutations: " + name)
    let retainedBytes = try Data(contentsOf: file)
    var retained = stat()
    guard lstat(file.path, &retained) == 0 else {
        throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
    }
    try check(retainedBytes == originalBytes && retained.st_dev == original.st_dev &&
              retained.st_ino == original.st_ino,
              "A mutation-policy decision must preserve original bytes and file identity: " + name)
}

@main
struct SocietyFSMetadataTests {
    static func main() throws {
        let manager = FileManager.default
        let root = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
            .appendingPathComponent("fskit-metadata-" + UUID().uuidString, isDirectory: true)
        let nested = root.appendingPathComponent("user folder", isDirectory: true)
        try manager.createDirectory(at: nested, withIntermediateDirectories: true)
        defer { try? manager.removeItem(at: root) }
        let rootFD = open(root.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        let nestedFD = open(nested.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        try check(rootFD >= 0 && nestedFD >= 0, "The fixture directories must open")
        defer { close(rootFD); close(nestedFD) }

        let volumeNames = [".fseventsd", ".Spotlight-V100"]
        for name in volumeNames {
            try check(societyMetadataMutationIsBlocked(name: name, atVolumeRoot: true,
                                                       insideProtectedDirectory: false),
                      "Missing root management entries must reject all mutations")
            try expectNewMetadataRejection(directory: rootFD, name: name, atVolumeRoot: true)
            try check(!manager.fileExists(atPath: root.appendingPathComponent(name).path),
                      "A rejected destination must not leave a file or folder")
            let nestedReserved = try societyExistingMetadataDestination(directory: nestedFD, name: name, atVolumeRoot: false)
            try check(!nestedReserved, "A same-name nested user folder must remain allowed")
            try check(!societyMetadataMutationIsBlocked(name: name, atVolumeRoot: false,
                                                        insideProtectedDirectory: false),
                      "A same-name folder below an ordinary user directory must remain writable")

            let ordinaryFolder = nested.appendingPathComponent(name, isDirectory: true)
            try manager.createDirectory(at: ordinaryFolder, withIntermediateDirectories: false)
            let ordinaryPayload = ordinaryFolder.appendingPathComponent("user.txt")
            try Data("nested original bytes".utf8).write(to: ordinaryPayload)
            try Data("nested updated bytes".utf8).write(to: ordinaryPayload)
            let ordinaryBytes = try Data(contentsOf: ordinaryPayload)
            try check(ordinaryBytes == Data("nested updated bytes".utf8),
                      "Ordinary nested same-name user folders must retain writes")

            let existing = root.appendingPathComponent(name, isDirectory: true)
            try manager.createDirectory(at: existing, withIntermediateDirectories: false)
            let payload = existing.appendingPathComponent("user.txt")
            try Data("original user data".utf8).write(to: payload)
            var originalDirectory = stat()
            try check(lstat(existing.path, &originalDirectory) == 0, "The existing management folder must be readable")
            let isExisting = try societyExistingMetadataDestination(directory: rootFD, name: name, atVolumeRoot: true)
            try check(isExisting, "Existing management entries must remain visible and readable")
            try check(societyMetadataMutationIsBlocked(name: name, atVolumeRoot: true,
                                                       insideProtectedDirectory: false),
                      "An existing root management folder must reject mutations")
            try expectProtectedFile(payload, name: "user.txt", atVolumeRoot: false,
                                    insideProtectedDirectory: true)
            for childName in ["new.txt", "deep-user-folder", ".Trashes", ".TemporaryItems",
                              ".DocumentRevisions-V100", ".DS_Store"] {
                try check(societyMetadataMutationIsBlocked(name: childName, atVolumeRoot: false,
                                                           insideProtectedDirectory: true),
                          "Every child of a protected management folder must reject mutations")
            }
            var retainedDirectory = stat()
            try check(lstat(existing.path, &retainedDirectory) == 0 &&
                      retainedDirectory.st_dev == originalDirectory.st_dev &&
                      retainedDirectory.st_ino == originalDirectory.st_ino,
                      "Mutation policy must preserve the protected directory's identity")
            let listedChildren = try manager.contentsOfDirectory(atPath: existing.path)
            try check(listedChildren == ["user.txt"], "Protected children must remain listed without added management files")
        }

        for (directory, url, isRoot) in [(rootFD, root, true), (nestedFD, nested, false)] {
            try check(societyMetadataMutationIsBlocked(name: ".DS_Store", atVolumeRoot: isRoot,
                                                       insideProtectedDirectory: false),
                      "Missing .DS_Store entries must reject mutations at every depth")
            try expectNewMetadataRejection(directory: directory, name: ".DS_Store", atVolumeRoot: isRoot)
            let existing = url.appendingPathComponent(".DS_Store")
            try Data("user-supplied bytes".utf8).write(to: existing)
            let isExisting = try societyExistingMetadataDestination(directory: directory, name: ".DS_Store", atVolumeRoot: isRoot)
            try check(isExisting, "Existing user-supplied .DS_Store files must remain visible and readable")
            try expectProtectedFile(existing, name: ".DS_Store", atVolumeRoot: isRoot,
                                    insideProtectedDirectory: false)
            let listedNames = try manager.contentsOfDirectory(atPath: url.path)
            try check(listedNames.contains(".DS_Store"), "Protected .DS_Store entries must remain listed")
        }
        for name in [".env", ".private", ".fseventsd-project", ".DS_Store.backup", "document.txt",
                     ".Trashes", ".TemporaryItems", ".DocumentRevisions-V100"] {
            let isReserved = try societyExistingMetadataDestination(directory: rootFD, name: name, atVolumeRoot: true)
            try check(!isReserved, "Only exact management names may be reserved")
            for isRoot in [true, false] {
                try check(!societyMetadataMutationIsBlocked(name: name, atVolumeRoot: isRoot,
                                                            insideProtectedDirectory: false),
                          "Ordinary user names and recovery/save folders must remain writable")
            }
        }
        let rootNames = Set(try manager.contentsOfDirectory(atPath: root.path))
        try check(Set(volumeNames + [".DS_Store"]).isSubset(of: rootNames),
                  "Mutation protection must not hide pre-existing root management names")
        do {
            _ = try societyExistingMetadataDestination(directory: -1, name: ".DS_Store", atVolumeRoot: false)
            throw NSError(domain: "SocietyFSMetadataTests", code: 3)
        } catch let error as POSIXError {
            try check(error.code == .EBADF, "I/O errors must retain their cause rather than count as a missing entry")
        }
        print("FSKit metadata: management mutations and descendant mutations rejected; existing bytes, identities, and listings preserved; ordinary user folders remain writable.")
    }
}
