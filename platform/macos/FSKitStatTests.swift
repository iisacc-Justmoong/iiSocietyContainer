import Foundation
import Darwin

private func check(_ condition: @autoclosure () -> Bool, _ message: String) throws {
    if !condition() { throw NSError(domain: "SocietyFSStatTests", code: 1,
                                   userInfo: [NSLocalizedDescriptionKey: message]) }
}

@main
struct SocietyFSStatTests {
    static func main() throws {
        var synthetic = stat()
        synthetic.st_blocks = 8
        for preferredIOSize: Int32 in [4096, 16384] {
            synthetic.st_blksize = preferredIOSize
            try check(societyAllocatedSize(synthetic) == 4096,
                      "Allocated bytes must use 512-byte sectors independently of the preferred I/O size")
        }
        synthetic.st_blocks = 0
        try check(societyAllocatedSize(synthetic) == 0, "An unallocated file must occupy zero bytes")

        let manager = FileManager.default
        let root = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
            .appendingPathComponent("fskit-stat-" + UUID().uuidString, isDirectory: true)
        try manager.createDirectory(at: root, withIntermediateDirectories: false)
        defer { try? manager.removeItem(at: root) }

        let ordinary = root.appendingPathComponent("ordinary.bin")
        try Data((0..<65536).map { UInt8($0 % 251) }).write(to: ordinary)
        let empty = root.appendingPathComponent("empty.bin")
        try Data().write(to: empty)
        let sparse = root.appendingPathComponent("sparse.bin")
        try Data().write(to: sparse)
        let handle = try FileHandle(forWritingTo: sparse)
        try handle.truncate(atOffset: 1 << 30)
        try handle.seek(toOffset: (1 << 30) - 1)
        try handle.write(contentsOf: Data([1]))
        try handle.synchronize()
        try handle.close()

        for url in [ordinary, empty, sparse] {
            var status = stat()
            try check(lstat(url.path, &status) == 0, "The fixture must have readable host allocation statistics")
            let values = try url.resourceValues(forKeys: [.totalFileAllocatedSizeKey])
            guard let allocated = values.totalFileAllocatedSize else {
                throw NSError(domain: "SocietyFSStatTests", code: 2,
                              userInfo: [NSLocalizedDescriptionKey: "Foundation must expose the fixture allocation"])
            }
            try check(societyAllocatedSize(status) == UInt64(allocated),
                      "Allocated bytes must match Foundation for " + url.lastPathComponent)
            if url == sparse {
                try check(status.st_size == 1 << 30, "The sparse fixture must retain its logical size")
                try check(societyAllocatedSize(status) < UInt64(status.st_size),
                          "A sparse file's allocation must remain smaller than its logical size")
            }
        }
        print("FSKit allocation: sector units, preferred I/O sizes, empty, ordinary and sparse files passed.")
    }
}
