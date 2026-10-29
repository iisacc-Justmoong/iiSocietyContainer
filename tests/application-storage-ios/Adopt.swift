import Foundation

// Execute the same native entry point used by Society's iOS drive bridge.
@_cdecl("storage_probe_adopt")
public func storageProbeAdopt() -> Int32 {
    do {
        _ = try SharedDriveLocation.current().prepareRoot()
        return 1
    } catch {
        print("Native Society adoption failed: \(error)")
        return 0
    }
}
