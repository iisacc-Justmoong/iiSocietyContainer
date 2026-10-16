import Foundation

@main
enum DirectoryMountTests {
    struct Failure: Error { let message: String }
    static func check(_ condition: @autoclosure () throws -> Bool, _ message: String) throws {
        guard try condition() else { throw Failure(message: message) }
    }
    static func reject(_ work: () throws -> Void) throws {
        do { try work() } catch { return }
        throw Failure(message: "Unsafe mount directory was accepted")
    }
    static func main() throws {
        let manager = FileManager.default
        let base = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
        let fixture = base.appendingPathComponent("mount-location-" + UUID().uuidString, isDirectory: true)
        try manager.createDirectory(at: fixture, withIntermediateDirectories: false)
        defer { try? manager.removeItem(at: fixture) }
        let root = fixture.appendingPathComponent("Society.societycontainer", isDirectory: true)
        try manager.createDirectory(at: root, withIntermediateDirectories: false)
        let identifier = UUID().uuidString.lowercased()
        let expected = root.appendingPathComponent(".society-runtime/mounts/" + identifier + "/Society")
        try check(DirectoryMount.location(source: root, identifier: identifier).path == expected.path,
                  "The system mount directory must belong to its source package")
        try check(manager.contentsOfDirectory(atPath: root.path).isEmpty, "Reading a mount location must not create files")
        let point = try DirectoryMount.prepareLocation(source: root, identifier: identifier)
        try check(point.path == expected.path && manager.contentsOfDirectory(atPath: point.path).isEmpty,
                  "Mount preparation must create only the empty in-package backing directory")
        try check(DirectoryMount.prepareLocation(source: root, identifier: identifier) == point,
                  "An unmounted empty backing directory must be reusable")
        try Data("retain user file".utf8).write(to: point.appendingPathComponent("user.txt"))
        try reject { _ = try DirectoryMount.prepareLocation(source: root, identifier: identifier) }
        try check(Data(contentsOf: point.appendingPathComponent("user.txt")) == Data("retain user file".utf8),
                  "A nonempty mount directory must preserve its files on rejection")
        try manager.removeItem(at: root.appendingPathComponent(".society-runtime"))
        let outside = fixture.appendingPathComponent("outside", isDirectory: true)
        try manager.createDirectory(at: outside, withIntermediateDirectories: false)
        try manager.createSymbolicLink(at: root.appendingPathComponent(".society-runtime"), withDestinationURL: outside)
        try reject { _ = try DirectoryMount.prepareLocation(source: root, identifier: identifier) }
        try check(manager.contentsOfDirectory(atPath: outside.path).isEmpty,
                  "A redirected runtime directory must never create files outside its package")
        try reject { _ = try DirectoryMount.location(source: outside, identifier: identifier) }
        try reject { _ = try DirectoryMount.location(source: root, identifier: "../outside") }
        let mount = DirectoryMount.MountedVolume(type: "societyfs", source: root.absoluteString,
                                                  point: "/Volumes/Society", identifier: identifier.uppercased())
        try check(DirectoryMount.existingVolume(source: root, identifier: identifier, volumes: [mount]) == mount,
                  "The actual filesystem UUID must find an OS-managed mount regardless of its mount point")
        try check(DirectoryMount.existingVolume(source: root, identifier: UUID().uuidString, volumes: [mount]) == nil,
                  "A different UUID must not reuse a Society mount")
        try reject { _ = try DirectoryMount.existingVolume(source: root, identifier: identifier, volumes: [mount, mount]) }
        let other = fixture.appendingPathComponent("Other.societycontainer", isDirectory: true)
        try manager.createDirectory(at: other, withIntermediateDirectories: false)
        try reject { _ = try DirectoryMount.existingVolume(source: other, identifier: identifier, volumes: [mount]) }
        try check(!DirectoryMount.needsSourceReload(mount, source: root), "An unchanged source must retain its active resource")
        let moved = fixture.appendingPathComponent("Renamed.societycontainer", isDirectory: true)
        try manager.moveItem(at: root, to: moved)
        try check(DirectoryMount.existingVolume(source: moved, identifier: identifier, volumes: [mount]) == mount,
                  "A renamed source inode must reconnect using the actual volume UUID")
        try check(DirectoryMount.needsSourceReload(mount, source: moved),
                  "A renamed source must renew its path resource grant before reporting a usable drive")
        try check(!manager.fileExists(atPath: moved.appendingPathComponent(".society-runtime/mounts").path),
                  "UUID discovery must never create a runtime or external mount directory")
        print("Legacy package preparation, actual UUID discovery, rename reconnect and collision protection passed")
    }
}
