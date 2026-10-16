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
        let root = base.appendingPathComponent("native-test-" + UUID().uuidString + ".societycontainer")
        let manager = FileManager.default
        try manager.createDirectory(at: root, withIntermediateDirectories: false)
        defer { _ = IisaccFileProvider.waitForPresentations(); try? manager.removeItem(at: root) }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: args[2])
        process.arguments = ["create", root.path]
        process.standardOutput = Pipe()
        try process.run(); process.waitUntilExit()
        try check(process.terminationStatus == 0, "SDK drive creation failed")
        let catalog = try JSONDecoder().decode([DriveSection].self, from: Data(contentsOf: base.appendingPathComponent("Sections.json")))
        let store = try LocalDriveStore(root: root, catalog: catalog)
        try check(store.manifest.displayName == "Society", "The native drive name must be Society")
        let bare = base.appendingPathComponent("bare-" + UUID().uuidString)
        try manager.createDirectory(at: bare, withIntermediateDirectories: false)
        defer { try? manager.removeItem(at: bare) }
        try rejects("An ordinary directory must not be managed as a native container") {
            try LocalDriveStore.validateSourceLocation(bare)
        }
        try check(manager.contentsOfDirectory(atPath: bare.path).isEmpty, "Rejected directories must remain untouched")
        for marker in ["Info.plist", "bands", ".society-disk.plist"] {
            let legacyMarker = root.appendingPathComponent(marker)
            try Data("legacy disk image".utf8).write(to: legacyMarker)
            try rejects("Legacy image markers must prevent native package registration") {
                _ = try LocalDriveStore(root: root, catalog: catalog)
            }
            try manager.removeItem(at: legacyMarker)
        }
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
        let nested = root.appendingPathComponent("Files/Nested.societycontainer")
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
        let replica = fakeHome.appendingPathComponent("Library/CloudStorage/Society.societycontainer")
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
        try LocalDriveStore.validateSourceLocation(fakeHome.appendingPathComponent("Library/CloudStorage-backup.societycontainer"), home: fakeHome)
        let initial = try store.snapshot()
        try check(initial.records.count == 9 + 1, "A new lazy working set must contain only the nine section roots and drive root")
        try check(store.children("root").count == 9, "Nine section directories must be enumerable")
        try check(store.children("section:models").isEmpty, "Unused model categories must not be materialized")
        try check(manager.contentsOfDirectory(atPath: root.appendingPathComponent("Models").path).isEmpty,
                  "Opening a new store must leave the physical Models folder empty")
        // Existing user categories remain enumerable; the fixture supplies them
        // explicitly rather than expecting drive creation to materialize them.
        let modelCategories = ["Checkpoint", "Embedding", "Hypernetwork", "Aesthetic Gradient",
                               "LoRA", "LyCORIS", "DoRA", "Controlnet", "Upscaler", "Motion", "VAE", "Text Encoder",
                               "UNet", "CLIP Vision", "Poses", "Wildcards", "Workflows", "ComfyUI Workflows",
                               "Detection", "VLM", "CLIP", "LLM", "Other"]
        for name in modelCategories {
            try manager.createDirectory(at: root.appendingPathComponent("Models/" + name), withIntermediateDirectories: false)
        }
        let existingCategories = try store.children("section:models")
        try check(Set(existingCategories.map { $0.name }) == Set(modelCategories), "All 23 explicitly supplied categories must remain enumerable")
        try check(store.snapshot().records.count == 9 + 23 + 1, "The working set must retain all explicitly supplied model categories")
        let sentinel = root.appendingPathComponent("Files/.iisacc-temporary.lock")
        try Data().write(to: sentinel)
        try check(!store.snapshot().records.values.contains { $0.name == ".iisacc-temporary.lock" }, "Temporary lease metadata must not be enumerable")
        try rejects("Native creation must protect the lease sentinel") {
            _ = try store.create(parent: "section:files", name: ".iisacc-temporary.lock", directory: false, contents: nil)
        }
        try check(Data(contentsOf: sentinel).isEmpty, "Public operations must preserve the empty sentinel")
        try check(store.children("section:models").count == 23, "Models must preserve all 23 user category directories")
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
        // Repeated derived writes do not create public revisions or evict the
        // caller's historical anchor. Visible child changes have their own rows.
        for index in 0..<10 {
            try Data("derived \(index)".utf8).write(to: root.appendingPathComponent("Files/.historical.iisacc-preview"))
            try check(store.snapshot().anchor == anchor.anchor, "Derived previews must not create public directory revisions")
        }
        let folder = try store.create(parent: "section:models", name: "Nested", directory: true, contents: nil)
        let moved = try store.modify(file.id, name: "renamed.txt", parent: folder.id, contents: nil,
                                     baseContentVersion: nil, baseMetadataVersion: file.metadataVersion)
        try check(moved.id == file.id && moved.path == "Models/Nested/renamed.txt", "Rename and move must preserve item identity")
        let reopened = try LocalDriveStore(root: root, catalog: catalog)
        try check(reopened.item(file.id).path == moved.path, "Item IDs must survive process restart")
        let reopenedCategories = try reopened.children("section:models")
        try check(existingCategories.allSatisfy { original in
            reopenedCategories.contains { $0.id == original.id && $0.path == original.path }
        }, "User model category identities and paths must survive reopening")
        try Data("replacement".utf8).write(to: upload, options: .atomic)
        let edited = try store.modify(file.id, name: nil, parent: nil, contents: upload,
                                      baseContentVersion: moved.contentVersion, baseMetadataVersion: nil)
        try check(edited.id == file.id && edited.contentVersion != moved.contentVersion, "Content replacement must preserve ID and change version")
        try check(edited.metadataVersion == moved.metadataVersion, "Content edits must not invalidate unchanged name and parent versions")
        let lease = root.appendingPathComponent(".iisacc-temporary.lock")
        try manager.removeItem(at: lease)
        try manager.createSymbolicLink(at: lease, withDestinationURL: upload)
        try rejects("Content replacement must reject a redirected maintenance lease") {
            _ = try store.modify(file.id, name: nil, parent: nil, contents: upload,
                                 baseContentVersion: edited.contentVersion, baseMetadataVersion: nil)
        }
        try check(Data(contentsOf: root.appendingPathComponent(edited.path)) == Data("replacement".utf8),
                  "A rejected maintenance lease must preserve existing content")
        try manager.removeItem(at: lease)
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
        // One native domain survives a logical host adoption and retains old
        // anchors so the OS can reconcile its previously cached file listing.
        let beforeMirror = try store.snapshot()
        var mirrorManifest = try JSONSerialization.jsonObject(with: originalManifest) as! [String: Any]
        let hostIdentifier = UUID().uuidString.lowercased()
        mirrorManifest["identifier"] = hostIdentifier
        mirrorManifest["localIdentifier"] = store.manifest.identifier
        mirrorManifest["replicaReady"] = false
        try JSONSerialization.data(withJSONObject: mirrorManifest).write(to: manifestURL, options: .atomic)
        try rejects("An old source instance must stop when logical identity changes") { _ = try store.snapshot() }
        try rejects("An incomplete mirror must not be exposed by a native adapter") { _ = try LocalDriveStore(root: root, catalog: catalog) }
        mirrorManifest["replicaReady"] = true
        try JSONSerialization.data(withJSONObject: mirrorManifest).write(to: manifestURL, options: .atomic)
        let mirrorStore = try LocalDriveStore(root: root, catalog: catalog)
        try check(mirrorStore.manifest.identifier == hostIdentifier, "The logical drive must match its host")
        try check(mirrorStore.manifest.providerIdentifier == store.manifest.identifier, "The native domain must stay stable")
        try check(mirrorStore.snapshot(at: beforeMirror.anchor) != nil, "Native history must survive identity adoption")
        _ = try mirrorStore.snapshot()
        let mirrorAgain = try LocalDriveStore(root: root, catalog: catalog)
        try check(mirrorAgain.manifest.identifier == hostIdentifier, "The adopted native drive must survive reopening")
        try manager.removeItem(at: root.appendingPathComponent("Files"))
        try manager.createSymbolicLink(at: root.appendingPathComponent("Files"), withDestinationURL: base)
        try rejects("Redirected section accepted after initialization") { _ = try store.snapshot() }
        print("PASS: native source creation, nine sections, CRUD, stable identity, restart, versions, anchors, boundaries and data preservation")
    }
}
