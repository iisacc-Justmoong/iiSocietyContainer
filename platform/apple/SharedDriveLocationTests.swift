import Foundation

@main
enum SharedDriveLocationTests {
    struct Failure: Error { let message: String }
    static func check(_ value: @autoclosure () throws -> Bool, _ message: String) throws {
        guard try value() else { throw Failure(message: message) }
    }
    static func reject(_ work: () throws -> Void) throws {
        do { try work() } catch { return }
        throw Failure(message: "Unsafe shared-container path was accepted")
    }
    static func main() throws {
        let manager = FileManager.default
        let base = URL(fileURLWithPath: CommandLine.arguments[1])
        let group = base.appendingPathComponent("app-group-test-" + UUID().uuidString)
        try manager.createDirectory(at: group, withIntermediateDirectories: false)
        defer { try? manager.removeItem(at: group) }
        let location = SharedDriveLocation(group: group)
        try reject { _ = try location.existingRoot() }
        let root = try location.prepareRoot()
        try check(root.path == group.appendingPathComponent("Library/Application Support/Society").path,
                  "The source must be private app-group storage")
        try check(location.prepareRoot() == root, "Reopening the app must preserve the same source")
        try reject { try location.validate(root: group.appendingPathComponent("Documents")) }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: CommandLine.arguments[2])
        process.arguments = ["create", root.path]
        process.standardOutput = Pipe()
        try process.run(); process.waitUntilExit()
        try check(process.terminationStatus == 0, "SDK drive creation failed")
        let catalog = try JSONDecoder().decode([DriveSection].self, from: Data(contentsOf: base.appendingPathComponent("Sections.json")))
        let app = try LocalDriveStore(root: root, catalog: catalog)
        let provider = try FilesDriveStore(root: location.existingRoot(), catalog: catalog)
        // Open both instances before writing: cached, independent indexes must not
        // mint different IDs or overwrite each other's snapshot history.
        let before = try provider.snapshot()
        let first = try app.create(parent: "section:files", name: "From Society.txt", directory: false, contents: nil)
        // Reproduce the iOS /var versus /private/var alias without depending on
        // which alias Foundation chooses on this host. Unequal prefix lengths
        // must not put an extra "Society" component into an item's parent path.
        let alias = base.appendingPathComponent("source-alias-" + UUID().uuidString)
        try manager.createSymbolicLink(at: alias, withDestinationURL: root)
        defer { try? manager.removeItem(at: alias) }
        try check(LocalDriveStore.relativePath(of: alias.appendingPathComponent("Files/From Society.txt"), under: root)
                  == "Files/From Society.txt", "Source aliases must produce the same relative item path")
        try reject { _ = try LocalDriveStore.relativePath(of: base, under: root) }
        try reject { _ = try LocalDriveStore.relativePath(of: root, under: root) }
        let history = root.appendingPathComponent("Generation History/Dreamscapes", isDirectory: true)
        try manager.createDirectory(at: history, withIntermediateDirectories: false)
        try check(LocalDriveStore.relativePath(of: alias.appendingPathComponent("Generation History/Dreamscapes"), under: root)
                  == "Generation History/Dreamscapes", "Nested private folders must retain their real parent")
        try check(provider.item("root").directory, "A nested private folder must not make the public root unavailable")
        try check(provider.item(first.id).name == first.name, "App-created IDs must survive extension discovery")
        let folder = try provider.create(parent: "root", name: "Models", directory: true, contents: nil)
        try check(app.item(folder.id).path == "Files/Models", "The public Models folder must remain in Files")
        let privateFile = try app.create(parent: "section:models", name: "Private.txt", directory: false, contents: nil)
        try reject { _ = try provider.item(privateFile.id) }
        try check(provider.previousSnapshot(forChangesFrom: before.anchor) != nil, "Another instance must retain sync history")
        let publicID = first.id
        let reopened = try FilesDriveStore(root: location.existingRoot(), catalog: catalog)
        try check(reopened.item(publicID).id == publicID, "Extension restart must preserve public IDs")
        try check(Set(reopened.children("root").map { $0.name }) == ["From Society.txt", "Models"],
                  "The iOS location must open directly in Files contents")
        try check(app.children("root").count == 8, "Society must retain all eight sections")
        try manager.removeItem(at: root)
        try manager.createSymbolicLink(at: root, withDestinationURL: group)
        try reject { _ = try location.prepareRoot() }
        try manager.removeItem(at: root)
        try manager.removeItem(at: group.appendingPathComponent("Library"))
        try manager.createSymbolicLink(at: group.appendingPathComponent("Library"), withDestinationURL: base)
        try reject { _ = try location.prepareRoot() }
        print("Shared app source, restart, stable IDs, Files projection and redirect checks passed")
    }
}
