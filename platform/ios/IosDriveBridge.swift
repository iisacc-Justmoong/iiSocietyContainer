import Foundation
import FileProvider
import UIKit
import UniformTypeIdentifiers

public typealias SocietyIosDriveCompletion = @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<CChar>?) -> Void

/// Runs in the containing app. No helper process or source bookmark is needed.
@_cdecl("society_ios_drive_request")
public func societyIosDriveRequest(_ action: UnsafePointer<CChar>, _ root: UnsafePointer<CChar>,
                                   _ identifier: UnsafePointer<CChar>, _ context: UnsafeMutableRawPointer?,
                                   _ completion: @escaping SocietyIosDriveCompletion) {
    let action = String(cString: action)
    let root = String(cString: root)
    let identifier = String(cString: identifier)
    func finish(_ result: [String: Any]) {
        DispatchQueue.main.async {
            let data = try! JSONSerialization.data(withJSONObject: result, options: [.sortedKeys])
            String(decoding: data, as: UTF8.self).withCString { completion(context, $0) }
        }
    }
    func failed(_ error: Error) { finish(["error": error.localizedDescription]) }
    DispatchQueue.main.async {
        do {
            let location = try SharedDriveLocation.current()
            if action == "default" {
                finish(["sourcePath": try location.prepareRoot().path])
                return
            }
            let source = URL(fileURLWithPath: root, isDirectory: true)
            try location.validate(root: source)
            guard let catalogURL = Bundle.main.url(forResource: "Sections", withExtension: "json") else {
                throw DriveStoreError.invalid("The Society section catalog is missing.")
            }
            let catalog = try JSONDecoder().decode([DriveSection].self, from: Data(contentsOf: catalogURL))
            let store = try LocalDriveStore(root: source, catalog: catalog)
            guard store.manifest.identifier == identifier else {
                throw DriveStoreError.invalid("The source does not match this drive.")
            }
            let domain = NSFileProviderDomain(identifier: .init(rawValue: identifier), displayName: store.manifest.displayName)
            if #available(iOS 18.0, *) {
                domain.supportsSyncingTrash = false
            }

            func connected(_ registered: NSFileProviderDomain) {
                guard let manager = NSFileProviderManager(for: registered) else {
                    failed(NSFileProviderError(.providerNotFound)); return
                }
                if !registered.userEnabled {
                    DispatchQueue.main.async {
                        if action == "path" {
                            do { try presentFiles(at: nil) } catch { failed(error); return }
                        }
                        finish(["enabled": false, "systemPath": ""])
                    }
                    return
                }
                manager.signalEnumerator(for: .workingSet) { error in
                    if let error = error { failed(error); return }
                    manager.getUserVisibleURL(for: .rootContainer) { url, error in
                        if let error = error { failed(error); return }
                        guard let url = url else { failed(NSFileProviderError(.noSuchItem)); return }
                        // A registered domain and a visible URL do not prove
                        // that the extension can enumerate the root. Ask the
                        // system to materialize it before reporting connected.
                        DispatchQueue.global(qos: .utility).async {
                            do {
                                guard url.startAccessingSecurityScopedResource() else {
                                    throw CocoaError(.fileReadNoPermission)
                                }
                                defer { url.stopAccessingSecurityScopedResource() }
                                var coordinationError: NSError?
                                var result: Result<Void, Error>?
                                NSFileCoordinator().coordinate(readingItemAt: url, options: [], error: &coordinationError) { visible in
                                    result = Result {
                                        _ = try FileManager.default.contentsOfDirectory(at: visible, includingPropertiesForKeys: nil)
                                    }
                                }
                                if let error = coordinationError { throw error }
                                guard let result = result else { throw CocoaError(.fileReadUnknown) }
                                try result.get()
                                DispatchQueue.main.async {
                                    if action == "path" {
                                        do { try presentFiles(at: url) } catch { failed(error); return }
                                    }
                                    finish(["systemPath": url.path, "enabled": registered.userEnabled])
                                }
                            } catch { failed(error) }
                        }
                    }
                }
            }
            func resolveRegisteredDomain() {
                NSFileProviderManager.getDomainsWithCompletionHandler { domains, error in
                    if let error = error { failed(error); return }
                    guard let registered = domains.first(where: { $0.identifier == domain.identifier }) else {
                        failed(NSFileProviderError(.providerNotFound)); return
                    }
                    connected(registered)
                }
            }
            if action == "register" {
                NSFileProviderManager.add(domain) { error in
                    if let error = error { failed(error) } else { resolveRegisteredDomain() }
                }
            } else if action == "refresh" || action == "path" {
                resolveRegisteredDomain()
            } else {
                throw DriveStoreError.invalid("Unknown iOS drive operation.")
            }
        } catch { failed(error) }
    }
}

/// UIKit's Files browser starts at the provider root, never at source storage.
@MainActor
private func presentFiles(at url: URL?) throws {
    let sceneWindows = UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene })
        .filter { $0.activationState == .foregroundActive }.flatMap { $0.windows }
    // Qt 6.8 can show a normal window without making it the key window, and
    // also supports the pre-scene UIApplication lifecycle.
    let windows = sceneWindows + UIApplication.shared.windows
    let window = windows.first(where: { $0.isKeyWindow && $0.rootViewController != nil })
        ?? windows.first(where: { !$0.isHidden && $0.alpha > 0 && $0.windowLevel == .normal && $0.rootViewController != nil })
    guard var controller = window?.rootViewController else {
        throw DriveStoreError.invalid("Society needs an active window to open Files.")
    }
    while let presented = controller.presentedViewController { controller = presented }
    let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.item], asCopy: false)
    picker.directoryURL = url
    picker.allowsMultipleSelection = false
    controller.present(picker, animated: true)
}
