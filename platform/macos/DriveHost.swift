import Foundation
import FileProvider

@main
enum DriveHost {
    static func main() {
        let arguments = Array(CommandLine.arguments.dropFirst())
        var finished = false
        var status: Int32 = 0
        let finish: ([String: Any]?, Error?) -> Void = { result, error in
            let complete = {
                guard !finished else { return }
                var output = result ?? [:]
                if let error = error {
                    output["error"] = error.localizedDescription
                    output["errorCode"] = (error as NSError).code
                    output["errorDomain"] = (error as NSError).domain
                    status = 1
                }
                if let data = try? JSONSerialization.data(withJSONObject: output, options: [.sortedKeys]) {
                    FileHandle.standardOutput.write(data)
                    FileHandle.standardOutput.write(Data([10]))
                }
                finished = true
            }
            if Thread.isMainThread { complete() } else { DispatchQueue.main.async(execute: complete) }
        }
        do {
            guard let command = arguments.first, ["register", "list", "path", "refresh", "unregister"].contains(command),
                  command == "list" || arguments.count == 2 else {
                throw DriveStoreError.invalid("Usage: SocietyContainerDrive register <source-directory> | list | path <drive-id> | refresh <drive-id> | unregister <drive-id>")
            }
            var newDomain: NSFileProviderDomain?
            var selectedID = arguments.count == 2 ? arguments[1] : ""
            if command == "register" {
                let root = URL(fileURLWithPath: arguments[1], isDirectory: true).resolvingSymlinksInPath()
                guard let catalogURL = Bundle.main.url(forResource: "Sections", withExtension: "json") else {
                    throw DriveStoreError.invalid("The section catalog is missing.")
                }
                let catalog = try JSONDecoder().decode([DriveSection].self, from: Data(contentsOf: catalogURL))
                let store = try LocalDriveStore(root: root, catalog: catalog)
                selectedID = store.manifest.identifier
                // App-scoped bookmarks cannot be resolved by a different process identity.
                // Transfer an implicit grant; the extension persists its own scoped bookmark.
                let bookmark = try root.bookmarkData(options: [.minimalBookmark], includingResourceValuesForKeys: nil, relativeTo: nil)
                let domain = NSFileProviderDomain(identifier: .init(rawValue: selectedID), displayName: store.manifest.displayName)
                domain.userInfo = [
                    "societyDriveIdentifier": selectedID, "sourceBookmark": bookmark, "sourcePath": root.path
                ]
                domain.supportsSyncingTrash = false
                newDomain = domain
            }
            let driveID = selectedID
            let registration = newDomain
            NSFileProviderManager.getDomainsWithCompletionHandler { domains, error in
                if let error = error { finish(nil, error); return }
                if command == "list" {
                    finish(["drives": domains.map { ["identifier": $0.userInfo?["societyDriveIdentifier"] as? String ?? "",
                                                     "domainIdentifier": $0.identifier.rawValue, "displayName": $0.displayName,
                                                     "sourcePath": $0.userInfo?["sourcePath"] as? String ?? ""] }], nil)
                    return
                }
                let existing = domains.first { $0.userInfo?["societyDriveIdentifier"] as? String == driveID }
                let report: (NSFileProviderDomain) -> Void = { domain in
                    guard let manager = NSFileProviderManager(for: domain) else { finish(nil, NSFileProviderError(.providerNotFound)); return }
                    manager.getUserVisibleURL(for: .rootContainer) { url, error in
                        finish(["identifier": driveID, "domainIdentifier": domain.identifier.rawValue,
                                "displayName": domain.displayName, "systemPath": url?.path ?? "",
                                "enabled": domain.userEnabled], error)
                    }
                }
                if command == "register", let domain = registration {
                    let reportRegistration: (Error?) -> Void = { error in
                        if let error = error { finish(nil, error); return }
                        // The newly constructed domain does not carry the system's
                        // userEnabled state; read back the registered domain.
                        NSFileProviderManager.getDomainsWithCompletionHandler { registered, error in
                            if let error = error { finish(nil, error); return }
                            guard let current = registered.first(where: {
                                $0.userInfo?["societyDriveIdentifier"] as? String == driveID
                            }) else { finish(nil, NSFileProviderError(.providerNotFound)); return }
                            report(current)
                        }
                    }
                    if let existing = existing {
                        guard existing.userInfo?["sourcePath"] as? String == domain.userInfo?["sourcePath"] as? String else {
                            finish(nil, DriveStoreError.invalid("This drive ID is already connected to a different source folder.")); return
                        }
                        // Re-adding the same domain ID updates its display name
                        // without removing the domain or its downloaded files.
                        let updated = NSFileProviderDomain(identifier: existing.identifier, displayName: domain.displayName)
                        updated.userInfo = domain.userInfo
                        updated.isHidden = existing.isHidden
                        updated.supportsSyncingTrash = false
                        NSFileProviderManager.add(updated, completionHandler: reportRegistration)
                    } else {
                        NSFileProviderManager.add(domain, completionHandler: reportRegistration)
                    }
                } else if let domain = existing {
                    if command == "unregister" {
                        NSFileProviderManager.remove(domain, mode: .preserveDownloadedUserData) { url, error in
                            finish(["identifier": driveID, "preservedPath": url?.path ?? ""], error)
                        }
                    } else if command == "refresh" {
                        guard let manager = NSFileProviderManager(for: domain) else { finish(nil, NSFileProviderError(.providerNotFound)); return }
                        manager.signalEnumerator(for: .workingSet) { error in
                            finish(["identifier": driveID], error)
                        }
                    } else { report(domain) }
                } else { finish(nil, DriveStoreError.invalid("This Society drive is not connected to Finder.")) }
            }
        } catch { finish(nil, error) }
        let deadline = Date().addingTimeInterval(30)
        while !finished && Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.05)) }
        if !finished { finish(nil, DriveStoreError.invalid("File Provider did not respond within 30 seconds.")) }
        exit(status)
    }
}
