import Foundation
import Darwin

// A request owns its directory stream. Reopening "." creates a separate open
// file description; dup would preserve the borrowed descriptor's shared offset.
final class SocietyFSDirectoryStream {
    private var directory: UnsafeMutablePointer<DIR>?
    private(set) var nextCookie: UInt64 = 0

    init(directory borrowedDirectory: Int32, startingAt cookie: UInt64 = 0) throws {
        let descriptor = openat(borrowedDirectory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        guard descriptor >= 0 else {
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
        guard let stream = fdopendir(descriptor) else {
            let failure = errno
            Darwin.close(descriptor)
            throw POSIXError(POSIXErrorCode(rawValue: failure) ?? .EIO)
        }
        directory = stream
        do {
            // Darwin telldir tokens belong to one DIR*. The request cookie is
            // instead the number of raw entries already consumed. Include dot
            // entries and entries hidden by the caller in this count.
            while nextCookie < cookie {
                guard try next() != nil else { throw POSIXError(.EINVAL) }
            }
        } catch {
            close()
            throw error
        }
    }

    // The returned entry is borrowed until the next next() or close() call.
    func next() throws -> UnsafeMutablePointer<dirent>? {
        guard let directory else { throw POSIXError(.EBADF) }
        errno = 0
        guard let entry = readdir(directory) else {
            let failure = errno
            if failure != 0 { throw POSIXError(POSIXErrorCode(rawValue: failure) ?? .EIO) }
            return nil
        }
        guard nextCookie < UInt64.max else { throw POSIXError(.EOVERFLOW) }
        nextCookie += 1
        return entry
    }

    func close() {
        guard let stream = directory else { return }
        directory = nil
        let failure = errno
        closedir(stream)
        errno = failure
    }

    deinit { close() }
}
