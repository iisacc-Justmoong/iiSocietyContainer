import Darwin
import Foundation
import Dispatch

private func check(_ condition: @autoclosure () -> Bool, _ message: String) throws {
    if !condition() { throw NSError(domain: "SocietyFSIOTests", code: 1,
                                   userInfo: [NSLocalizedDescriptionKey: message]) }
}

private func write(_ bytes: Data, descriptor: Int32) throws {
    let written = bytes.withUnsafeBytes { pwrite(descriptor, $0.baseAddress, $0.count, 0) }
    try check(written == bytes.count, "The operation descriptor must permit pwrite")
}

private func read(_ descriptor: Int32, count: Int) throws -> Data {
    var bytes = Data(count: count)
    let actual = bytes.withUnsafeMutableBytes { pread(descriptor, $0.baseAddress, $0.count, 0) }
    try check(actual == count, "The operation descriptor must permit pread")
    return bytes
}

@main
struct SocietyFSIOTests {
    static func main() throws {
        let manager = FileManager.default
        let root = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
            .appendingPathComponent("fskit-io-" + UUID().uuidString, isDirectory: true)
        try manager.createDirectory(at: root, withIntermediateDirectories: false)
        defer { try? manager.removeItem(at: root) }
        let file = root.appendingPathComponent("pre-existing.txt")
        let initial = Data("original user bytes".utf8)
        let updated = Data("modified user bytes".utf8)
        try initial.write(to: file)
        let cachedRead = open(file.path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)
        try check(cachedRead >= 0, "A source-preloaded file must open read-only")
        var original = stat()
        try check(fstat(cachedRead, &original) == 0, "The original identity must be available")
        let deniedWrite = updated.withUnsafeBytes { pwrite(cachedRead, $0.baseAddress, $0.count, 0) }
        try check(deniedWrite == -1 && errno == EBADF,
                  "Writing through the cached read-only descriptor reproduces the original failure")
        var reopenCount = 0
        let ownedWrite = try societyDescriptorForIO(cachedRead, writable: true) {
            reopenCount += 1
            return open(file.path, O_RDWR | O_NOFOLLOW | O_CLOEXEC)
        }
        defer { close(ownedWrite) }
        try check(reopenCount == 1, "A read-only cached file must reopen for a write operation")
        try check((fcntl(cachedRead, F_GETFL) & O_ACCMODE) == O_RDONLY,
                  "Borrowing a writer must not replace the cached reader")
        try check((fcntl(ownedWrite, F_GETFD) & FD_CLOEXEC) != 0,
                  "Operation descriptors must remain private across exec")
        close(cachedRead)
        try write(updated, descriptor: ownedWrite)
        var after = stat()
        try check(fstat(ownedWrite, &after) == 0 && original.st_dev == after.st_dev && original.st_ino == after.st_ino,
                  "Read-then-write must preserve the original user inode")
        let retainedUpdate = try Data(contentsOf: file)
        try check(retainedUpdate == updated, "The write must reach the pre-existing user file")

        let ownedRead = try societyDescriptorForIO(ownedWrite, writable: false) {
            throw POSIXError(.EIO)
        }
        try check(ownedRead != ownedWrite, "The operation must own an independent descriptor number")
        let projectedUpdate = try read(ownedRead, count: updated.count)
        try check(projectedUpdate == updated, "A writer may also supply a reader")
        close(ownedRead)

        let closedWrite = try societyDescriptorForIO(-1, writable: true) {
            open(file.path, O_RDWR | O_NOFOLLOW | O_CLOEXEC)
        }
        let activeWrite = try societyDescriptorForIO(closedWrite, writable: true) { throw POSIXError(.EIO) }
        close(closedWrite)
        try write(initial, descriptor: activeWrite)
        close(activeWrite)
        let retainedInitial = try Data(contentsOf: file)
        try check(retainedInitial == initial,
                  "Closing a cached item while I/O is active must not invalidate the operation")

        let baseline = fcntl(ownedWrite, F_DUPFD_CLOEXEC, 0)
        try check(baseline >= 0, "The descriptor baseline must be available")
        close(baseline)
        for _ in 0..<200 {
            let operation = try societyDescriptorForIO(ownedWrite, writable: true) { throw POSIXError(.EIO) }
            try write(initial, descriptor: operation)
            close(operation)
        }
        let next = fcntl(ownedWrite, F_DUPFD_CLOEXEC, 0)
        try check(next == baseline, "Repeated operations must not leak descriptors")
        close(next)

        let metadataCache = open(file.path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)
        try check(metadataCache >= 0, "The metadata cache must open")
        let metadataReaders = try (0..<16).map { _ in
            try societyDescriptorForIO(metadataCache, writable: false) { throw POSIXError(.EIO) }
        }
        close(metadataCache)
        let failuresLock = NSLock()
        var metadataFailures = 0
        let expectedDevice = original.st_dev
        let expectedInode = original.st_ino
        DispatchQueue.concurrentPerform(iterations: metadataReaders.count) { index in
            let descriptor = metadataReaders[index]
            var attributes = stat()
            let failed = fstat(descriptor, &attributes) != 0 ||
                attributes.st_dev != expectedDevice || attributes.st_ino != expectedInode
            if failed {
                failuresLock.lock(); metadataFailures += 1; failuresLock.unlock()
            }
            close(descriptor)
        }
        try check(metadataFailures == 0,
                  "Concurrent attribute requests must retain their own readable descriptor after cached close")
        do {
            _ = try societyDescriptorForIO(-1, writable: true) { throw POSIXError(.EACCES) }
            throw NSError(domain: "SocietyFSIOTests", code: 2)
        } catch let error as POSIXError {
            try check(error.code == .EACCES, "Reopen errors must preserve their original cause")
        }
        print("FSKit I/O: pre-existing read-then-write, concurrent cached close, identity and descriptor lifetime passed.")
    }
}
