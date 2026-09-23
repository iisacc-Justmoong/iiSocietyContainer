import Foundation
import FileProvider

private final class EnumerationResult: NSObject, NSFileProviderEnumerationObserver {
    var items: [NSFileProviderItem] = []
    var finished = false
    var error: Error?
    func didEnumerate(_ items: [NSFileProviderItem]) { self.items += items }
    func finishEnumerating(upTo nextPage: NSFileProviderPage?) { finished = nextPage == nil }
    func finishEnumeratingWithError(_ error: Error) { self.error = error }
}

private final class ChangeResult: NSObject, NSFileProviderChangeObserver {
    var items: [NSFileProviderItem] = []
    var deleted: [NSFileProviderItemIdentifier] = []
    var anchor: NSFileProviderSyncAnchor?
    var error: Error?
    func didUpdate(_ items: [NSFileProviderItem]) { self.items += items }
    func didDeleteItems(withIdentifiers identifiers: [NSFileProviderItemIdentifier]) { deleted += identifiers }
    func finishEnumeratingChanges(upTo anchor: NSFileProviderSyncAnchor, moreComing: Bool) { if !moreComing { self.anchor = anchor } }
    func finishEnumeratingWithError(_ error: Error) { self.error = error }
}

@main
enum FilesDriveStoreTests {
    struct Failure: Error { let message: String }
    static func check(_ condition: @autoclosure () throws -> Bool, _ message: String) throws {
        guard try condition() else { throw Failure(message: message) }
    }
    static func missing(_ message: String, _ work: () throws -> Void) throws {
        do { try work() } catch DriveStoreError.missing { return }
        throw Failure(message: message)
    }
    static func rejects(_ message: String, _ work: () throws -> Void) throws {
        do { try work() } catch { return }
        throw Failure(message: message)
    }
    static func main() throws {
        let base = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
        let root = base.appendingPathComponent("files-test-" + UUID().uuidString)
        let manager = FileManager.default
        try manager.createDirectory(at: root, withIntermediateDirectories: false)
        defer { try? manager.removeItem(at: root) }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: CommandLine.arguments[2])
        process.arguments = ["create", root.path]
        process.standardOutput = Pipe()
        try process.run(); process.waitUntilExit()
        try check(process.terminationStatus == 0, "SDK drive creation failed")
        let catalog = try JSONDecoder().decode([DriveSection].self, from: Data(contentsOf: base.appendingPathComponent("Sections.json")))
        for section in catalog {
            try Data(section.id.utf8).write(to: root.appendingPathComponent(section.path + "/Example.txt"))
        }
        try manager.createDirectory(at: root.appendingPathComponent("Files/Nested"), withIntermediateDirectories: false)
        try Data("nested".utf8).write(to: root.appendingPathComponent("Files/Nested/Child.txt"))
        try Data("unassigned".utf8).write(to: root.appendingPathComponent("Unassigned.txt"))
        let legacy = try LocalDriveStore(root: root, catalog: catalog).snapshot()
        let store = try FilesDriveStore(root: root, catalog: catalog)
        let initial = try store.snapshot()
        try check(store.root.path == root.appendingPathComponent("Files").path, "The public source root must be Society/Files")
        let expectedChildren: Set<String> = ["Example.txt", "Nested"]
        try check(Set(store.children("root").map { $0.name }) == expectedChildren, "Opening the disk must show only existing Files contents")
        try check(initial.records.count == 4, "The working set must contain only Files and its descendants")
        try check(initial.records.values.allSatisfy { !$0.id.hasPrefix("section:") && !$0.path.hasPrefix("Files/") }, "Internal section IDs and the Files prefix must not be exposed")
        let publicFile = try store.children("root").first { $0.name == "Example.txt" }!
        try check(publicFile.parent == "root" && publicFile.path == "Example.txt", "Files children must belong directly to the public root")
        try check(publicFile.id == legacy.records.values.first { $0.path == "Files/Example.txt" }?.id, "Upgrading must preserve public file IDs")
        let previous = store.previousSnapshot(forChangesFrom: legacy.anchor)!
        let retired = Set(previous.records.keys).subtracting(initial.records.keys)
        try check(retired.contains("section:files") && retired.count == 17 + 23, "Legacy changes must retire all nine section roots, seven private files, and 23 model categories")
        try check(store.previousSnapshot(forChangesFrom: initial.anchor)?.records == initial.records, "New anchors must return only the public snapshot")
        try check(store.previousSnapshot(forChangesFrom: "unknown") == nil, "Unknown anchors must expire")
        let rootEnumeration = EnumerationResult()
        let firstPage = NSFileProviderPage(NSFileProviderPage.initialPageSortedByName as Data)
        DriveEnumerator(source: { try FilesDriveStore(root: root, catalog: catalog) }, identifier: .rootContainer).enumerateItems(for: rootEnumeration, startingAt: firstPage)
        try check(rootEnumeration.finished && rootEnumeration.error == nil && Set(rootEnumeration.items.map { $0.filename }) == expectedChildren, "Finder root enumeration must contain Files children directly")
        let workingSet = DriveEnumerator(source: { try FilesDriveStore(root: root, catalog: catalog) }, identifier: .workingSet)
        let changes = ChangeResult()
        workingSet.enumerateChanges(for: changes, from: NSFileProviderSyncAnchor(Data(legacy.anchor.utf8)))
        try check(changes.error == nil && changes.anchor != nil && Set(changes.deleted.map { $0.rawValue }) == retired, "The provider must send all legacy private IDs as deletions")
        try check(changes.items.contains { $0.itemIdentifier == .rootContainer && $0.capabilities?.contains(.allowsAddingSubItems) == true }, "An existing connection must receive the writable public root capability")
        try check(changes.items.allSatisfy { $0.itemIdentifier == .rootContainer || initial.records[$0.itemIdentifier.rawValue] != nil }, "Change updates must never publish private records")
        let repeated = ChangeResult()
        workingSet.enumerateChanges(for: repeated, from: changes.anchor!)
        try check(repeated.items.isEmpty && repeated.deleted.isEmpty && repeated.anchor == changes.anchor, "Unchanged refresh must not publish spurious root updates")
        let complete = EnumerationResult()
        workingSet.enumerateItems(for: complete, startingAt: firstPage)
        try check(complete.items.contains { $0.itemIdentifier == .rootContainer } && complete.items.count == 4, "Full reconciliation must include root metadata and only public descendants")

        for name in ["Documents", "Audios", "3D objects"] {
            let directory = try store.create(parent: "root", name: name, directory: true, contents: nil)
            try check(!directory.protected, "Former default names must be ordinary user folders")
            let capabilities = DriveItem(directory).capabilities
            try check(capabilities.contains(.allowsDeleting) && capabilities.contains(.allowsRenaming)
                && capabilities.contains(.allowsReparenting), "User folders must expose normal mutation capabilities")
            let nested = initial.records.values.first { $0.path == "Nested" }!
            let moved = try store.modify(directory.id, name: name + " moved", parent: nested.id, contents: nil, baseContentVersion: nil, baseMetadataVersion: nil)
            try store.remove(moved.id, baseContentVersion: nil, baseMetadataVersion: nil)
            try check(!store.children("root").contains { $0.name == name }, "Deleted folders must not be restored")
            let file = try store.create(parent: "root", name: name, directory: false, contents: nil)
            try store.remove(file.id, baseContentVersion: nil, baseMetadataVersion: nil)
        }

        let upload = root.appendingPathComponent("Upload.txt")
        try Data("public edit".utf8).write(to: upload)
        let privateRecords = legacy.records.values.filter { $0.id != "root" && $0.path != "Files" && !$0.path.hasPrefix("Files/") }
        for record in privateRecords {
            try missing("Private identifiers must not be readable") { _ = try store.item(record.id) }
            try missing("Private identifiers must not be enumerable") { _ = try store.children(record.id) }
            try missing("Private contents must not be fetched") { _ = try store.copyContents(record.id, to: root) }
            try missing("Private items must not be modified") { _ = try store.modify(record.id, name: "changed", parent: "root", contents: nil, baseContentVersion: nil, baseMetadataVersion: nil) }
            try missing("Private items must not be removed") { try store.remove(record.id, baseContentVersion: nil, baseMetadataVersion: nil) }
            try missing("Private parents must reject creation") { _ = try store.create(parent: record.id, name: "Injected", directory: false, contents: upload) }
            try missing("Public items must not be moved into private areas") { _ = try store.modify(publicFile.id, name: nil, parent: record.id, contents: nil, baseContentVersion: nil, baseMetadataVersion: nil) }
        }
        try missing("Legacy Files section IDs must not create a second root") { _ = try store.item("section:files") }
        let folder = try store.create(parent: "root", name: "Models", directory: true, contents: nil)
        try check(folder.path == "Models" && manager.fileExists(atPath: root.appendingPathComponent("Files/Models").path), "A user folder named Models belongs inside Files")
        let created = try store.create(parent: "root", name: "Created.txt", directory: false, contents: upload)
        try check(Data(contentsOf: root.appendingPathComponent("Files/Created.txt")) == Data("public edit".utf8), "Root writes must reach Files")
        let renamed = try store.modify(created.id, name: "Renamed.txt", parent: folder.id, contents: nil, baseContentVersion: nil, baseMetadataVersion: created.metadataVersion)
        let returned = try store.modify(renamed.id, name: nil, parent: "root", contents: nil, baseContentVersion: nil, baseMetadataVersion: renamed.metadataVersion)
        try check(returned.path == "Renamed.txt" && returned.parent == "root", "Public metadata versions must work when moving to and from root")
        try rejects("Stale public metadata must not overwrite") { _ = try store.modify(returned.id, name: "Stale.txt", parent: nil, contents: nil, baseContentVersion: nil, baseMetadataVersion: created.metadataVersion) }
        try Data("replacement".utf8).write(to: upload, options: .atomic)
        let edited = try store.modify(returned.id, name: nil, parent: nil, contents: upload, baseContentVersion: returned.contentVersion, baseMetadataVersion: nil)
        try check(edited.metadataVersion == returned.metadataVersion, "A content write must not invalidate a queued rename or move")
        _ = try store.modify(edited.id, name: "After content write.txt", parent: nil, contents: nil, baseContentVersion: nil, baseMetadataVersion: returned.metadataVersion)
        try rejects("Stale public contents must not overwrite") { _ = try store.modify(edited.id, name: nil, parent: nil, contents: upload, baseContentVersion: returned.contentVersion, baseMetadataVersion: nil) }
        let copied = try store.copyContents(edited.id, to: root)
        try check(Data(contentsOf: copied) == Data("replacement".utf8), "Public fetch must return the current content")
        let latest = try store.item(edited.id)
        try store.remove(latest.id, baseContentVersion: latest.contentVersion, baseMetadataVersion: latest.metadataVersion)
        try check(!manager.fileExists(atPath: root.appendingPathComponent("Files/After content write.txt").path), "Public deletes must reach Files")
        _ = try store.create(parent: folder.id, name: "Child before deletion.txt", directory: false, contents: upload)
        try check(store.item(folder.id).contentVersion == folder.contentVersion, "Child writes must not invalidate the directory content version")
        try store.remove(folder.id, baseContentVersion: folder.contentVersion, baseMetadataVersion: folder.metadataVersion)
        try check(!manager.fileExists(atPath: root.appendingPathComponent("Files/Models").path)
                  && manager.fileExists(atPath: root.appendingPathComponent("Models/Example.txt").path), "Recursive deletion must remove the public folder and preserve the private area")
        for name in ["..", "../Models/escape", "/absolute", "bad\0name"] {
            try rejects("Invalid root child name accepted") { _ = try store.create(parent: "root", name: name, directory: false, contents: nil) }
        }
        _ = try store.modify("root", name: store.manifest.displayName, parent: "root", contents: nil, baseContentVersion: nil, baseMetadataVersion: nil)
        try rejects("Public root cannot be renamed") { _ = try store.modify("root", name: "changed", parent: nil, contents: nil, baseContentVersion: nil, baseMetadataVersion: nil) }
        try rejects("Public root cannot be removed") { try store.remove("root", baseContentVersion: nil, baseMetadataVersion: nil) }
        try manager.createSymbolicLink(at: root.appendingPathComponent("Files/Private Link"), withDestinationURL: root.appendingPathComponent("Models"))
        try check(!store.snapshot().records.values.contains { $0.name == "Private Link" }, "Links to private areas must be excluded")

        let beforeMove = try store.snapshot()
        try manager.moveItem(at: root.appendingPathComponent("Files/Example.txt"), to: root.appendingPathComponent("Models/Withdrawn.txt"))
        let afterMove = try store.snapshot()
        try check(afterMove.records[publicFile.id] == nil, "App moves out of Files must withdraw the public item")
        try missing("Withdrawn IDs must not fetch private contents") { _ = try store.copyContents(publicFile.id, to: root) }
        try check(store.previousSnapshot(forChangesFrom: beforeMove.anchor)?.records[publicFile.id] != nil, "Withdrawal must be present in the change history")
        try manager.moveItem(at: root.appendingPathComponent("Models/Withdrawn.txt"), to: root.appendingPathComponent("Files/Restored.txt"))
        try check(store.item(publicFile.id).path == "Restored.txt", "Publishing back to Files must retain identity")
        let reopened = try FilesDriveStore(root: root, catalog: catalog)
        try check(reopened.item(publicFile.id).path == "Restored.txt", "Public identity must survive provider restart")
        let oldAnchor = try reopened.snapshot().anchor
        let manifestURL = root.appendingPathComponent(".society-drive.json")
        var manifest = try JSONSerialization.jsonObject(with: Data(contentsOf: manifestURL)) as! [String: Any]
        manifest["localIdentifier"] = store.manifest.identifier
        manifest["identifier"] = UUID().uuidString.lowercased()
        manifest["replicaReady"] = false
        try JSONSerialization.data(withJSONObject: manifest).write(to: manifestURL, options: .atomic)
        let pending = EnumerationResult()
        workingSet.enumerateItems(for: pending, startingAt: firstPage)
        try check(pending.error != nil && pending.items.isEmpty, "Existing enumerators must withhold incomplete mirrors")
        try manager.removeItem(at: root.appendingPathComponent("Files/Restored.txt"))
        try Data("host bytes".utf8).write(to: root.appendingPathComponent("Files/Host.txt"))
        manifest["replicaReady"] = true
        try JSONSerialization.data(withJSONObject: manifest).write(to: manifestURL, options: .atomic)
        let adoptedChanges = ChangeResult()
        workingSet.enumerateChanges(for: adoptedChanges, from: NSFileProviderSyncAnchor(Data(oldAnchor.utf8)))
        try check(adoptedChanges.error == nil, "Existing enumerators must reopen an adopted mirror")
        try check(adoptedChanges.deleted.contains { $0.rawValue == publicFile.id }, "The old independent file must leave the native replica")
        try check(adoptedChanges.items.contains { $0.filename == "Host.txt" }, "Host files must enter the existing native domain")
        let appStore = try LocalDriveStore(root: root, catalog: catalog)
        try check(appStore.children("root").count == 9, "Society must retain all nine logical areas")
        for section in catalog where section.id != "files" {
            try check(Data(contentsOf: root.appendingPathComponent(section.path + "/Example.txt")) == Data(section.id.utf8), "Private content must remain intact for Society")
        }
        print("FilesDriveStore: direct root, private access denial, CRUD, versions, migration, app access and restart passed")
    }
}
