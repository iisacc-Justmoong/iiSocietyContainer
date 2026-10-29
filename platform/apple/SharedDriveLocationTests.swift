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
        defer {
            _ = IisaccFileProvider.waitForPresentations()
            try? manager.removeItem(at: group)
        }
        let redirectedGroup = base.appendingPathComponent("guard-group-" + UUID().uuidString)
        let sentinel = base.appendingPathComponent("guard-target-" + UUID().uuidString)
        try manager.createDirectory(at: redirectedGroup, withIntermediateDirectories: false)
        try Data("unchanged".utf8).write(to: sentinel)
        defer { try? manager.removeItem(at: redirectedGroup); try? manager.removeItem(at: sentinel) }
        try manager.createSymbolicLink(at: redirectedGroup.appendingPathComponent(".iisacc-storage.guard"), withDestinationURL: sentinel)
        try reject { _ = try SharedDriveLocation(group: redirectedGroup).prepareRoot() }
        try check(Data(contentsOf: sentinel) == Data("unchanged".utf8), "The ownership guard must never follow a redirected target")
        let location = SharedDriveLocation(group: group)
        try reject { _ = try location.existingRoot() }
        let root = try location.prepareRoot()
        let guardAttributes = try manager.attributesOfItem(atPath: group.appendingPathComponent(".iisacc-storage.guard").path)
        try check((guardAttributes[.posixPermissions] as? NSNumber)?.intValue == 0o600,
                  "The provider-owned guard must retain private permissions")

        try check(root.path == group.appendingPathComponent("Society.societycontainer").path,
                  "The source must be private app-group storage")
        try check(Set(manager.contentsOfDirectory(atPath: group.path)) == ["Society.societycontainer", ".iisacc-storage.guard"],
                  "Preparing the shared source must create one package without outside scaffolding")
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
        try check(app.children("root").count == 9, "Society must retain all nine sections")
        try check(IisaccFileProvider.waitForPresentations(),
                  "File previews must finish before relocating the shared-source fixture")
        let previous = group.appendingPathComponent("Library/Application Support/Society", isDirectory: true)
        try manager.createDirectory(at: previous.deletingLastPathComponent(), withIntermediateDirectories: true)
        let manifestBytes = try Data(contentsOf: root.appendingPathComponent(".society-drive.json"))
        try manager.moveItem(at: root, to: previous)
        try reject { _ = try location.existingRoot() }
        try check(location.prepareRoot() == root, "The previous managed directory must move into one package")
        try check(!manager.fileExists(atPath: previous.path), "Moving the original must not retain a second managed source")
        try check(Data(contentsOf: root.appendingPathComponent(".society-drive.json")) == manifestBytes,
                  "Packaging the previous managed source must preserve the manifest and UUID")
        try check(FilesDriveStore(root: root, catalog: catalog).item(publicID).id == publicID,
                  "Packaging a previous managed source must preserve user files and provider IDs")
        try check(IisaccFileProvider.waitForPresentations(),
                  "File previews must finish before replacing the shared-source fixture")
        try manager.removeItem(at: root)
        try manager.createSymbolicLink(at: root, withDestinationURL: group)
        try reject { _ = try location.prepareRoot() }
        try manager.removeItem(at: root)
        try manager.createSymbolicLink(at: root, withDestinationURL: base)
        try reject { _ = try location.prepareRoot() }
        // Dreamscapes is the first app on a clean shared container. Society's
        // native owner and File Provider later adopt the same source in place.
        let logicalGroup = base.appendingPathComponent("logical-app-group-" + UUID().uuidString)
        try IisaccFileProvider.createDirectory(at: logicalGroup, withIntermediateDirectories: false)
        defer { _ = IisaccFileProvider.waitForPresentations(); try? manager.removeItem(at: logicalGroup) }
        func sdkApplication() throws -> [String: Any] {
            let process = Process(); let pipe = Pipe()
            process.executableURL = URL(fileURLWithPath: CommandLine.arguments[2])
            process.arguments = ["application", logicalGroup.path, "com.iisacc.dreamscapes"]
            process.standardOutput = pipe; try process.run()
            let data = pipe.fileHandleForReading.readDataToEndOfFile(); process.waitUntilExit()
            try check(process.terminationStatus == 0, "Application-owned SDK storage failed")
            return try JSONSerialization.jsonObject(with: data) as! [String: Any]
        }
        let application = try sdkApplication()
        let logicalRoot = URL(fileURLWithPath: application["rootPath"] as! String, isDirectory: true)
        let logicalLocation = SharedDriveLocation(group: logicalGroup)
        let registration = logicalGroup.appendingPathComponent("Library/Application Support/iisacc/Society/storage.json")
        func owner() throws -> String { (try JSONSerialization.jsonObject(with: Data(contentsOf: registration)) as! [String: Any])["managingApplication"] as! String }
        try check(logicalRoot.pathExtension != "societycontainer", "The app disk must not require a directory-package name")
        try check(logicalLocation.existingRoot() == logicalRoot, "The extension must discover the app's logical disk")
        try check(owner() == "com.iisacc.dreamscapes", "Read-only discovery must preserve the application owner")
        let clientStore = try LocalDriveStore(root: logicalRoot, catalog: catalog)
        let original = base.appendingPathComponent("logical-original-" + UUID().uuidString + ".png")
        try IisaccFileProvider.write(Data("generated image".utf8), to: original)
        defer { _ = IisaccFileProvider.waitForPresentations(); try? manager.removeItem(at: original) }
        let generated = try clientStore.create(parent: "section:generation-history", name: "generated.png", directory: false,
                                              contents: original)
        let sharedFile = try clientStore.create(parent: "section:files", name: "From Dreamscapes.txt", directory: false,
                                               contents: original)
        try check(logicalLocation.prepareRoot() == logicalRoot, "Society adoption must preserve the source path")
        try check(owner() == "com.iisacc.society", "Society must take over the shared registration")
        let societyStore = try LocalDriveStore(root: logicalLocation.existingRoot(), catalog: catalog)
        let publicStore = try FilesDriveStore(root: logicalLocation.existingRoot(), catalog: catalog)
        try check(societyStore.manifest.identifier == application["identifier"] as! String, "Adoption must preserve the logical UUID")
        try check(societyStore.item(generated.id).id == generated.id, "Generated-image IDs must survive adoption")
        try check(publicStore.item(sharedFile.id).id == sharedFile.id, "Files IDs must survive File Provider discovery")
        try check(!manager.fileExists(atPath: logicalGroup.appendingPathComponent("Society.societycontainer").path), "Adoption must not create a duplicate disk")
        let reopenedApplication = try sdkApplication()
        try check(reopenedApplication["rootPath"] as! String == logicalRoot.path && owner() == "com.iisacc.society", "The originating app must reopen Society's adopted storage")
        print("Shared app source, restart, stable IDs, Files projection and redirect checks passed")
    }
}
