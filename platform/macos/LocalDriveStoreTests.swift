import Foundation

@main
enum LocalDriveStoreTests {
    struct Failure: Error { let message: String }
    static func check(_ condition: @autoclosure () throws -> Bool, _ message: String) throws {
        guard try condition() else { throw Failure(message: message) }
    }
    static func rejects(_ message: String, _ work: () throws -> Void) throws {
        do { try work() } catch { return }
        throw Failure(message: message)
    }
    static func main() throws {
        let args = CommandLine.arguments
        let base = URL(fileURLWithPath: args[1], isDirectory: true)
        let root = base.appendingPathComponent("native-test-" + UUID().uuidString)
        let manager = FileManager.default
        try manager.createDirectory(at: root, withIntermediateDirectories: false)
        defer { try? manager.removeItem(at: root) }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: args[2])
        process.arguments = ["create", root.path]
        process.standardOutput = Pipe()
        try process.run(); process.waitUntilExit()
        try check(process.terminationStatus == 0, "SDK drive creation failed")
        let catalog = try JSONDecoder().decode([DriveSection].self, from: Data(contentsOf: base.appendingPathComponent("Sections.json")))
        let store = try LocalDriveStore(root: root, catalog: catalog)
        try check(store.manifest.displayName == "Society", "The native drive name must be Society")
        let manifestURL = root.appendingPathComponent(".society-drive.json")
        let originalManifest = try Data(contentsOf: manifestURL)
        var legacyManifest = try JSONSerialization.jsonObject(with: originalManifest) as! [String: Any]
        legacyManifest["displayName"] = "Society Container"
        let legacyData = try JSONSerialization.data(withJSONObject: legacyManifest, options: .sortedKeys)
        try legacyData.write(to: manifestURL, options: .atomic)
        let legacyStore = try LocalDriveStore(root: root, catalog: catalog)
        try check(legacyStore.manifest.identifier == store.manifest.identifier, "Renaming must preserve the drive ID")
        try check(legacyStore.manifest.displayName == "Society" && legacyStore.item("root").name == "Society", "Legacy roots must display Society")
        try check(Data(contentsOf: manifestURL) == legacyData, "Reading a legacy drive must not rewrite its manifest")
        try originalManifest.write(to: manifestURL, options: .atomic)
        for options: URL.BookmarkCreationOptions in [.minimalBookmark, .withSecurityScope] {
            let bookmark = try root.bookmarkData(options: options, includingResourceValuesForKeys: nil, relativeTo: nil)
            var stale = false
            let granted = try URL(resolvingBookmarkData: bookmark, options: [.withoutUI], relativeTo: nil, bookmarkDataIsStale: &stale)
            let bookmarkStore = try LocalDriveStore(root: granted, catalog: catalog)
            try check(bookmarkStore.manifest.identifier == store.manifest.identifier, "Bookmark-backed source validation must terminate and preserve identity")
        }
        let nested = root.appendingPathComponent("Files/Nested")
        try manager.createDirectory(at: nested, withIntermediateDirectories: false)
        for section in catalog {
            try manager.createDirectory(at: nested.appendingPathComponent(section.path), withIntermediateDirectories: false)
        }
        try manager.copyItem(at: root.appendingPathComponent(".society-drive.json"), to: nested.appendingPathComponent(".society-drive.json"))
        try rejects("A previously initialized nested source must be rejected") {
            _ = try LocalDriveStore(root: nested, catalog: catalog)
        }
        try check(!manager.fileExists(atPath: nested.appendingPathComponent(".society-drive-provider.json").path), "Rejection must not write a provider index")
        try manager.removeItem(at: nested)
        let fakeHome = base.appendingPathComponent("home-" + UUID().uuidString)
        let replica = fakeHome.appendingPathComponent("Library/CloudStorage/SocietyContainer-SocietyContainer")
        try manager.createDirectory(at: replica, withIntermediateDirectories: true)
        defer { try? manager.removeItem(at: fakeHome) }
        for url in [replica, replica.appendingPathComponent("Nested/Deep")] {
            try rejects("A Finder replica cannot be a native source") {
                try LocalDriveStore.validateSourceLocation(url, home: fakeHome)
            }
        }
        let alias = fakeHome.appendingPathComponent("Alias")
        try manager.createSymbolicLink(at: alias, withDestinationURL: replica)
        try rejects("A symlink cannot bypass the Finder replica boundary") {
            try LocalDriveStore.validateSourceLocation(alias, home: fakeHome)
        }
        try LocalDriveStore.validateSourceLocation(fakeHome.appendingPathComponent("Library/CloudStorage-backup"), home: fakeHome)
        let initial = try store.snapshot()
        try check(initial.records.count == 9, "The root must contain exactly eight sections")
        try check(store.children("root").count == 8, "Eight section directories must be enumerable")
        for section in catalog {
            let folder = try store.create(parent: "section:" + section.id, name: "Example", directory: true, contents: nil)
            try check(folder.parent == "section:" + section.id, "Every section must support identical operations")
            try store.remove(folder.id, baseContentVersion: nil, baseMetadataVersion: nil)
        }
        let upload = base.appendingPathComponent("upload-" + UUID().uuidString)
        defer { try? manager.removeItem(at: upload) }
        try Data("original".utf8).write(to: upload)
        let file = try store.create(parent: "section:files", name: "한글 # %.txt", directory: false, contents: upload)
        let anchor = try store.snapshot()
        let folder = try store.create(parent: "section:models", name: "Nested", directory: true, contents: nil)
        let moved = try store.modify(file.id, name: "renamed.txt", parent: folder.id, contents: nil,
                                     baseContentVersion: nil, baseMetadataVersion: file.metadataVersion)
        try check(moved.id == file.id && moved.path == "Models/Nested/renamed.txt", "Rename and move must preserve item identity")
        let reopened = try LocalDriveStore(root: root, catalog: catalog)
        try check(reopened.item(file.id).path == moved.path, "Item IDs must survive process restart")
        try Data("replacement".utf8).write(to: upload, options: .atomic)
        let edited = try store.modify(file.id, name: nil, parent: nil, contents: upload,
                                      baseContentVersion: moved.contentVersion, baseMetadataVersion: nil)
        try check(edited.id == file.id && edited.contentVersion != moved.contentVersion, "Content replacement must preserve ID and change version")
        try check(edited.metadataVersion == moved.metadataVersion, "Content edits must not invalidate unchanged name and parent versions")
        let downloaded = try store.copyContents(file.id, to: base)
        defer { try? manager.removeItem(at: downloaded) }
        try check(Data(contentsOf: downloaded) == Data("replacement".utf8), "Fetched content must match source")
        try check(downloaded.path != root.appendingPathComponent(edited.path).path, "Fetch must return an independent copy")
        try rejects("Stale writes must preserve current content") {
            _ = try store.modify(file.id, name: nil, parent: nil, contents: upload, baseContentVersion: moved.contentVersion, baseMetadataVersion: nil)
        }
        try check(store.snapshot(at: anchor.anchor)?.records[file.id]?.path == file.path, "Historical anchors must preserve prior records")
        for parent in ["root", "section:files"] {
            let unchanged = try store.modify(parent, name: nil, parent: nil, contents: nil, baseContentVersion: nil, baseMetadataVersion: nil)
            try check(unchanged.id == parent, "Incidental fixed-folder metadata updates must succeed")
            try rejects("Fixed roots cannot be deleted") { try store.remove(parent, baseContentVersion: nil, baseMetadataVersion: nil) }
            try rejects("Fixed roots cannot be renamed") { _ = try store.modify(parent, name: "changed", parent: nil, contents: nil, baseContentVersion: nil, baseMetadataVersion: nil) }
        }
        try rejects("Cannot add a ninth section") { _ = try store.create(parent: "root", name: "Other", directory: true, contents: nil) }
        for name in ["../escape", "a/b", "..", "", "bad\0name"] {
            try rejects("Invalid name accepted") { _ = try store.create(parent: "section:files", name: name, directory: false, contents: nil) }
        }
        try rejects("Collision must not overwrite") { _ = try store.create(parent: folder.id, name: edited.name, directory: false, contents: upload) }
        try rejects("A folder cannot be moved into itself") { _ = try store.modify(folder.id, name: nil, parent: folder.id, contents: nil, baseContentVersion: nil, baseMetadataVersion: nil) }
        try rejects("Invalid directory content replacement must not rename first") {
            _ = try store.modify(folder.id, name: "incorrect", parent: nil, contents: upload, baseContentVersion: nil, baseMetadataVersion: nil)
        }
        try check(store.item(folder.id).name == "Nested", "Rejected directory update changed its name")
        try Data("unassigned".utf8).write(to: root.appendingPathComponent("unassigned.txt"))
        try manager.createSymbolicLink(at: root.appendingPathComponent("Files/link"), withDestinationURL: upload)
        try check(!store.snapshot().records.values.contains { $0.name == "unassigned.txt" || $0.name == "link" }, "Unassigned root files and symlinks must not leak into the drive")
        try manager.moveItem(at: root.appendingPathComponent(edited.path), to: root.appendingPathComponent("Published/external.txt"))
        try Data("new occupant".utf8).write(to: root.appendingPathComponent(edited.path))
        try check(store.item(file.id).path == "Published/external.txt", "External moves must preserve identity")
        try store.remove(file.id, baseContentVersion: nil, baseMetadataVersion: nil)
        try check(store.snapshot().records[file.id] == nil, "Deletion must reach enumeration")
        try manager.removeItem(at: root.appendingPathComponent("Files"))
        try manager.createSymbolicLink(at: root.appendingPathComponent("Files"), withDestinationURL: base)
        try rejects("Redirected section accepted after initialization") { _ = try store.snapshot() }
        print("PASS: native source creation, eight sections, CRUD, stable identity, restart, versions, anchors, boundaries and data preservation")
    }
}
