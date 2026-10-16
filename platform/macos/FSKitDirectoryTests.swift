import Foundation
import Darwin

private func check(_ condition: @autoclosure () -> Bool, _ message: String) throws {
    if !condition() {
        throw NSError(domain: "SocietyFSDirectoryTests", code: 1,
                      userInfo: [NSLocalizedDescriptionKey: message])
    }
}

private func name(of entry: UnsafeMutablePointer<dirent>) -> String {
    withUnsafePointer(to: &entry.pointee.d_name) {
        $0.withMemoryRebound(to: CChar.self, capacity: Int(entry.pointee.d_namlen) + 1) {
            String(cString: $0)
        }
    }
}

private func remainingNames(in listing: SocietyFSDirectoryStream) throws -> [String] {
    var names: [String] = []
    while let entry = try listing.next() { names.append(name(of: entry)) }
    return names
}

private func names(directory: Int32, startingAt cookie: UInt64 = 0) throws -> [String] {
    let listing = try SocietyFSDirectoryStream(directory: directory, startingAt: cookie)
    defer { listing.close() }
    return try remainingNames(in: listing)
}

private func openDescriptorCount() -> Int {
    var count = 0
    for descriptor in 0..<getdtablesize() {
        if fcntl(descriptor, F_GETFD) >= 0 { count += 1 }
    }
    return count
}

private func expectError(_ expected: POSIXErrorCode, _ body: () throws -> Void) throws {
    do {
        try body()
        throw NSError(domain: "SocietyFSDirectoryTests", code: 2,
                      userInfo: [NSLocalizedDescriptionKey: "Expected POSIX error: \(expected)"])
    } catch let error as POSIXError {
        try check(error.code == expected, "The original POSIX error must be reported")
    }
}

private enum FixtureError: Error { case interrupted }

private func releaseAfterOneEntry(directory: Int32, throwing: Bool) throws {
    let listing = try SocietyFSDirectoryStream(directory: directory)
    _ = try listing.next()
    if throwing { throw FixtureError.interrupted }
    // Exercise deinit on an unfinished listing as well as explicit close().
}

private func pagedNames(directory: Int32, pageSize: Int) throws -> [String] {
    var cookie: UInt64 = 0
    var result: [String] = []
    while true {
        let listing = try SocietyFSDirectoryStream(directory: directory, startingAt: cookie)
        defer { listing.close() }
        var reachedEnd = false
        for _ in 0..<pageSize {
            guard let entry = try listing.next() else { reachedEnd = true; break }
            result.append(name(of: entry))
            try check(listing.nextCookie > cookie, "A packed entry must advance its request cookie")
            cookie = listing.nextCookie
        }
        if reachedEnd { return result }
    }
}

@main
struct SocietyFSDirectoryTests {
    static func main() throws {
        let manager = FileManager.default
        let root = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
            .appendingPathComponent("fskit-directory-" + UUID().uuidString, isDirectory: true)
        let nested = root.appendingPathComponent("nested", isDirectory: true)
        try manager.createDirectory(at: nested, withIntermediateDirectories: true)
        defer { try? manager.removeItem(at: root) }

        // Long names force several libc read buffers instead of testing only
        // the first fdopendir buffer, which can hide shared-offset failures.
        var expected: Set<String> = [".", "..", "nested"]
        for index in 0..<1024 {
            let filename = "entry-\(index)-" + String(repeating: "x", count: 96) + ".txt"
            try check(manager.createFile(atPath: root.appendingPathComponent(filename).path, contents: Data()),
                      "A fixture file must be created")
            expected.insert(filename)
        }
        for filename in [".DS_Store", ".iisacc-temporary.lock", ".env", "한글 사용자 파일.txt"] {
            try check(manager.createFile(atPath: root.appendingPathComponent(filename).path, contents: Data()),
                      "A same-name user fixture must be created")
            expected.insert(filename)
        }
        for folder in [".fseventsd", ".Spotlight-V100", ".TemporaryItems"] {
            try manager.createDirectory(at: root.appendingPathComponent(folder, isDirectory: true),
                                        withIntermediateDirectories: false)
            expected.insert(folder)
        }
        try Data("nested user content".utf8).write(to: nested.appendingPathComponent("user.txt"))

        let rootFD = open(root.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        guard rootFD >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { close(rootFD) }
        let originalOffset = lseek(rootFD, 0, SEEK_CUR)
        try check(originalOffset >= 0, "The borrowed directory offset must be observable")
        let baseline = try names(directory: rootFD)
        try check(Set(baseline) == expected && baseline.count == expected.count,
                  "Raw enumeration must preserve every user name without duplicates")
        try check(lseek(rootFD, 0, SEEK_CUR) == originalOffset,
                  "An owned stream must leave the borrowed directory offset unchanged")

        for _ in 0..<8 {
            let repeated = try names(directory: rootFD)
            try check(repeated == baseline, "Repeated requests must return the complete unchanged directory")
        }

        // Interleave requests after one has already read part of its stream.
        // Finishing another request must not move the first request's offset.
        let first = try SocietyFSDirectoryStream(directory: rootFD)
        defer { first.close() }
        var firstNames: [String] = []
        for _ in 0..<31 {
            guard let entry = try first.next() else { throw FixtureError.interrupted }
            firstNames.append(name(of: entry))
        }
        let secondNames = try names(directory: rootFD)
        firstNames += try remainingNames(in: first)
        try check(firstNames == baseline && secondNames == baseline,
                  "Interleaved streams must independently return every entry")
        first.close()

        let outer = try SocietyFSDirectoryStream(directory: rootFD)
        defer { outer.close() }
        guard let outerEntry = try outer.next() else { throw FixtureError.interrupted }
        var outerNames = [name(of: outerEntry)]
        let inner = try SocietyFSDirectoryStream(directory: rootFD)
        _ = try inner.next()
        inner.close()
        inner.close()
        try expectError(.EBADF) { _ = try inner.next() }
        outerNames += try remainingNames(in: outer)
        try check(outerNames == baseline, "Closing a nested request must preserve its surrounding request")
        outer.close()
        var rootAttributes = stat()
        try check(fstat(rootFD, &rootAttributes) == 0, "Closing request streams must preserve the borrowed descriptor")

        let nestedFD = openat(rootFD, "nested", O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        guard nestedFD >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        let nestedListing = try SocietyFSDirectoryStream(directory: nestedFD)
        close(nestedFD)
        let nestedNames = try remainingNames(in: nestedListing)
        nestedListing.close()
        try check(Set(nestedNames) == [".", "..", "user.txt"],
                  "An owned request must remain usable after its borrowed descriptor closes")

        for pageSize in [1, 2, 17, 257] {
            let paged = try pagedNames(directory: rootFD, pageSize: pageSize)
            try check(paged == baseline, "Fresh request streams must resume cookies without omissions or duplicates")
        }
        for offset in [1, baseline.count / 2, baseline.count - 1, baseline.count] {
            let suffix = try names(directory: rootFD, startingAt: UInt64(offset))
            try check(suffix == Array(baseline.dropFirst(offset)),
                      "A raw ordinal cookie must resume at the exact next entry, including EOF")
        }
        try expectError(.EINVAL) { _ = try names(directory: rootFD, startingAt: UInt64(baseline.count + 1)) }
        try expectError(.EINVAL) { _ = try names(directory: rootFD, startingAt: UInt64.max) }
        try expectError(.EBADF) { _ = try SocietyFSDirectoryStream(directory: -1) }
        let regularFD = openat(rootFD, ".env", O_RDONLY | O_CLOEXEC)
        guard regularFD >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { close(regularFD) }
        try expectError(.ENOTDIR) { _ = try SocietyFSDirectoryStream(directory: regularFD) }

        try releaseAfterOneEntry(directory: rootFD, throwing: false)
        let descriptorsBefore = openDescriptorCount()
        for _ in 0..<128 {
            try releaseAfterOneEntry(directory: rootFD, throwing: false)
            do {
                try releaseAfterOneEntry(directory: rootFD, throwing: true)
                throw NSError(domain: "SocietyFSDirectoryTests", code: 3)
            } catch FixtureError.interrupted {}
            try expectError(.EINVAL) { _ = try SocietyFSDirectoryStream(directory: rootFD, startingAt: UInt64.max) }
        }
        let descriptorsAfter = openDescriptorCount()
        try check(descriptorsAfter == descriptorsBefore,
                  "Normal, early, throwing, and invalid-cookie requests must not accumulate descriptors")
        try check(lseek(rootFD, 0, SEEK_CUR) == originalOffset,
                  "Pagination and failed requests must not move the borrowed directory offset")

        print("FSKit directory: independent request offsets, complete repeated/interleaved listings, ordinal pagination, and descriptor lifetime passed.")
    }
}
