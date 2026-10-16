import Foundation
import Darwin

// These names belong to volume services, except for Finder's per-folder store.
// Existing entries remain visible and readable; this policy never hides them.
func societyMetadataNameIsReserved(_ name: String, atVolumeRoot: Bool) -> Bool {
    if name == ".DS_Store" { return true }
    guard atVolumeRoot else { return false }
    return name == ".fseventsd" || name == ".Spotlight-V100"
}

// Protect existing bytes as well as rejecting new management destinations.
// The caller carries the protected-directory state through descendant items.
func societyMetadataMutationIsBlocked(name: String, atVolumeRoot: Bool,
                                       insideProtectedDirectory: Bool) -> Bool {
    insideProtectedDirectory || societyMetadataNameIsReserved(name, atVolumeRoot: atVolumeRoot)
}

// Return true for a pre-existing reserved entry. A missing reserved destination
// fails before a create or rename can forward management bytes into Files/.
func societyExistingMetadataDestination(directory: Int32, name: String,
                                         atVolumeRoot: Bool) throws -> Bool {
    guard societyMetadataNameIsReserved(name, atVolumeRoot: atVolumeRoot) else { return false }
    var attributes = stat()
    if fstatat(directory, name, &attributes, AT_SYMLINK_NOFOLLOW) == 0 { return true }
    let failure = errno
    if failure == ENOENT { throw POSIXError(.EPERM) }
    throw POSIXError(POSIXErrorCode(rawValue: failure) ?? .EIO)
}
