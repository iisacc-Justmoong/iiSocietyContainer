import Foundation
import Darwin

@main
enum FSKitRootTests {
    static func main() {
        do { try run() }
        catch {
            fputs("Society FSKit root test failed: \(error)\n", stderr)
            exit(1)
        }
    }
    static func run() throws {
        let manager = FileManager.default
        let parent = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
        let fixture = parent.appendingPathComponent("files-root-" + UUID().uuidString, isDirectory: true)
        try manager.createDirectory(at: fixture, withIntermediateDirectories: false)
        defer { try? manager.removeItem(at: fixture) }
        let container = fixture.appendingPathComponent("Society.societycontainer", isDirectory: true)
        let files = container.appendingPathComponent("Files", isDirectory: true)
        let models = container.appendingPathComponent("Models", isDirectory: true)
        try manager.createDirectory(at: files, withIntermediateDirectories: true)
        try manager.createDirectory(at: models, withIntermediateDirectories: false)
        let id = UUID()
        let manifest: [String: Any] = ["type": "SocietyDrive", "schemaVersion": 1, "identifier": id.uuidString]
        try JSONSerialization.data(withJSONObject: manifest).write(to: container.appendingPathComponent(".society-drive.json"))
        try Data("public".utf8).write(to: files.appendingPathComponent("public.txt"))
        try Data("private".utf8).write(to: models.appendingPathComponent("private.txt"))
        guard try societyFilesRoot(container) == files,
              try societyVolumeIdentifier(files.path) == id,
              try manager.contentsOfDirectory(atPath: societyFilesRoot(container).path) == ["public.txt"] else {
            throw NSError(domain: "SocietyFSKitRoot", code: 1)
        }
        try manager.moveItem(at: files, to: container.appendingPathComponent("Files.saved"))
        try manager.createSymbolicLink(at: files, withDestinationURL: models)
        var rejected = false
        do { _ = try societyFilesRoot(container) } catch { rejected = true }
        guard rejected else { throw NSError(domain: "SocietyFSKitRoot", code: 2) }

        let logical = fixture.appendingPathComponent("com.iisacc.dreamscapes", isDirectory: true)
        let logicalFiles = logical.appendingPathComponent("Files", isDirectory: true)
        try manager.createDirectory(at: logicalFiles, withIntermediateDirectories: true)
        try JSONSerialization.data(withJSONObject: manifest).write(to: logical.appendingPathComponent(".society-drive.json"))
        let declaration = logical.appendingPathComponent(".society-logical-disk")
        try Data("iisacc.logical-disk.v1\ncom.iisacc.dreamscapes\n".utf8).write(to: declaration)
        guard try societyFilesRoot(logical) == logicalFiles,
              try societyVolumeIdentifier(logicalFiles.path) == id else {
            throw NSError(domain: "SocietyFSKitRoot", code: 3)
        }
        var replica = manifest
        replica["localIdentifier"] = UUID().uuidString
        replica["replicaReady"] = false
        try JSONSerialization.data(withJSONObject: replica).write(to: logical.appendingPathComponent(".society-drive.json"))
        rejected = false
        do { _ = try societyFilesRoot(logical) } catch { rejected = true }
        guard rejected else { throw NSError(domain: "SocietyFSKitRoot", code: 5) }
        replica["replicaReady"] = true
        try JSONSerialization.data(withJSONObject: replica).write(to: logical.appendingPathComponent(".society-drive.json"))
        guard try societyFilesRoot(logical) == logicalFiles else { throw NSError(domain: "SocietyFSKitRoot", code: 6) }
        try manager.removeItem(at: declaration)
        rejected = false
        do { _ = try societyFilesRoot(logical) } catch { rejected = true }
        guard rejected else { throw NSError(domain: "SocietyFSKitRoot", code: 4) }
        print("The mounted root projects only the original Files directory and preserves its UUID")
    }
}
