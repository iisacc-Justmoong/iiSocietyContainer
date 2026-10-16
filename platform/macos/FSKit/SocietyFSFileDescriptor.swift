import Darwin
import Foundation

// The caller serializes access to its cached descriptor. The returned descriptor
// belongs to this one I/O operation, so closing the cached item cannot close it.
func societyDescriptorForIO(_ cachedDescriptor: Int32, writable: Bool,
                            reopen: () throws -> Int32) throws -> Int32 {
    if cachedDescriptor >= 0 {
        let flags = fcntl(cachedDescriptor, F_GETFL)
        guard flags >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        let access = flags & O_ACCMODE
        if writable ? access != O_RDONLY : access != O_WRONLY {
            let owned = fcntl(cachedDescriptor, F_DUPFD_CLOEXEC, 0)
            guard owned >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
            return owned
        }
    }
    let owned = try reopen()
    guard owned >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
    return owned
}
