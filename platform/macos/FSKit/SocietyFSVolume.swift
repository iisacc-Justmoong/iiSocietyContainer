/*
See the LICENSE.txt file for this sample’s licensing information.

Abstract:
A class defines a custom volume for use by the society file system.
*/

import Foundation
import ExtensionFoundation
import FSKit
import OSLog

let maxSymlinkSize: Int = 4096
let modeAllBits: Int32 = 0o7777

/// A SocietyFSVolume represents a volume in the society file system.
class SocietyFSVolume: FSVolume,
                           FSVolume.ReadWriteOperations,
                           FSVolume.RenameOperations,
                           FSVolume.PreallocateOperations,
                           FSVolume.OpenCloseOperations {

    /// The root item of the volume.
    var rootItem: SocietyFSItem

    /// The item cache stores items previously looked up or created;
    /// items are removed from the dictionary when the volume reclaims or removes the item.
    var itemCache: [UInt64: SocietyFSItem]

    /// The item cache is accessed concurrently so the volume needs to serialize access to it.
    var itemCacheQueue: DispatchQueue

    /// Creates a new SocietyFSVolume.
    /// - Parameter rootPath: The path to the root directory of the volume.
    init(rootPath: String) throws {
        let rootFD      = try throwErrno { Darwin.open(rootPath, O_RDONLY) }
        self.rootItem   = SocietyFSItem(name: ".", fileDescriptor: rootFD, type: .directory, openFlags: .readOnly)
        self.itemCache = [:]
        self.itemCacheQueue = DispatchQueue(label: "com.apple.fskit.societyfs.itemcache.queue")
        super.init(volumeID: FSVolume.Identifier(uuid: try societyVolumeIdentifier(rootPath)), volumeName: createVolumeNameFromPath(rootPath))
        Logger.societyfs.info("\(#function): Created a new volume with ID(\(self.volumeID)) and name(\(self.name)) on path(\(rootPath))")
    }

    /// The SocietyFS file system doesn't support setting a volume name, so this method does nothing and invokes its reply handler.
    public func setVolumeName(_ name: FSFileName, replyHandler: @escaping (FSFileName?, (any Error)?) -> Void) {
        return replyHandler(name, nil)
    }

    /// Prealocates disk space for the given item using `fcntl`.
    /// - Parameters:
    ///   - item: The item to preallocate space for.
    ///   - offset: The file offset at which to preallocate space.
    ///   - length: The length of the preallocated space.
    ///   - flags: The preallocation flags.
    ///   - replyHandler: The reply handler to invoke with the result.
    public func preallocateSpace(for item: FSItem,
                                 at offset: off_t,
                                 length: Int,
                                 flags: FSVolume.PreallocateFlags,
                                 replyHandler: @escaping (Int, (any Error)?) -> Void) {
        guard let ptItem = item as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast item")
            return replyHandler(0, POSIXError(.EINVAL))
        }
        guard ptItem.itemType == .file else {
            Logger.societyfs.error("\(#function): Can only preallocate a file")
            return replyHandler(0, POSIXError(.EPERM))
        }
        guard !ptItem.isReadOnlyMetadata else { return replyHandler(0, POSIXError(.EPERM)) }

        var preallocStruct = fstore_t()
        preallocStruct.fst_bytesalloc = 0
        preallocStruct.fst_flags = UInt32(flags.rawValue)
        preallocStruct.fst_length = Int64(length)
        preallocStruct.fst_offset = Int64(offset)
        preallocStruct.fst_posmode = F_PEOFPOSMODE

        let oldFD = ptItem.fileDescriptor
        if oldFD < 0 {
            try? ptItem.upgradeOpenMode(mode: .readWrite)
        }
        var err: Error?
        if fcntl(ptItem.fileDescriptor, F_PREALLOCATE, &preallocStruct) == -1 {
            err = posixErrno
        }
        if oldFD < 0 {
            try? ptItem.closeItem()
        }
        guard err == nil else {
            return replyHandler(0, err)
        }
        return replyHandler(Int(preallocStruct.fst_bytesalloc), nil)
    }

    /// Reads the contents of the given file item using `pread`.
    /// - Parameters:
    ///   - item: The file item to read from.
    ///   - offset: The file offset at which to begin reading.
    ///   - length: The number of bytes to read.
    ///   - buffer: The buffer into which to read the data.
    ///   - replyHandler: The reply handler to invoke with the result.
    public func read(from item: FSItem,
                     at offset: off_t,
                     length: Int,
                     into buffer: FSMutableFileDataBuffer,
                     replyHandler: @escaping (Int, Error?) -> Void) {
        guard let ptItem = item as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast item")
            return replyHandler(0, POSIXError(.EINVAL))
        }
        let descriptor: Int32
        do {
            descriptor = try ptItem.descriptorForIO(mode: .readOnly)
        } catch {
            return replyHandler(0, error)
        }
        defer { Darwin.close(descriptor) }
        var err: Error?
        var actuallyRead = 0
        buffer.withUnsafeMutableBytes { rawBufferPointer in
            actuallyRead = pread(descriptor, rawBufferPointer.baseAddress, length, offset)

            // Check if the read operation was successful.
            if actuallyRead == -1 {
                err = posixErrno
            }
        }

        guard err == nil else {
            return replyHandler(0, err)
        }
        return replyHandler(actuallyRead, nil)

    }

    /// Writes contents to the given file item using `pwrite`.
    /// - Parameters:
    ///   - contents: The data to write to the file item.
    ///   - item: The file item to write to.
    ///   - offset: The file offset at which to begin writing.
    ///   - replyHandler: The reply handler to invoke with the result.
    public func write(contents: Data,
                      to item: FSItem,
                      at offset: off_t,
                      replyHandler: @escaping (Int, (any Error)?) -> Void) {
        guard let ptItem = item as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast item")
            return replyHandler(0, POSIXError(.EINVAL))
        }

        guard ptItem.itemType != .directory else {
            Logger.societyfs.error("\(#function): Can't write to a folder")
            return replyHandler(0, POSIXError(.EISDIR))
        }
        guard !ptItem.isReadOnlyMetadata else { return replyHandler(0, POSIXError(.EPERM)) }

        let descriptor: Int32
        do {
            descriptor = try ptItem.descriptorForIO(mode: .readWrite)
        } catch {
            return replyHandler(0, error)
        }
        defer { IisaccFileProvider.closeAfterWriting(descriptor) }

        let bytesPtr: UnsafeMutablePointer<UInt8> = UnsafeMutablePointer<UInt8>.allocate(capacity: contents.count)
        contents.copyBytes(to: bytesPtr, count: contents.count)

        var err: Error?
        let actuallyWritten = pwrite(descriptor, bytesPtr, contents.count, off_t(offset))
        bytesPtr.deallocate()
        if actuallyWritten == -1 {
            err = posixErrno
        }
        guard err == nil else {
            return replyHandler(0, err)
        }
        return replyHandler(actuallyWritten, nil)
    }

    /// Performs an `open` operation on the given file item.
    /// - Parameters:
    ///   - item: The file item to open.
    ///   - modes: The open modes.
    ///   - replyHandler: The reply handler to invoke with the result.
    public func openItem(_ item: FSItem,
                         modes: FSVolume.OpenModes,
                         replyHandler: @escaping ((any Error)?) -> Void) {
        guard let ptItem = item as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast item")
            return replyHandler(POSIXError(.EINVAL))
        }
        if modes.contains(.write) && ptItem.isReadOnlyMetadata {
            return replyHandler(POSIXError(.EPERM))
        }
        guard ptItem != self.rootItem else {
            // root item is opened when creating the volume.
            return replyHandler(nil)
        }

        var ptfsMode: SocietyFSItemOpenMode = .close
        if modes.contains(.read) {
            ptfsMode = .readOnly
        }
        if modes.contains(.write) {
            ptfsMode = .readWrite
        }

        do {
            try ptItem.upgradeOpenMode(mode: ptfsMode)
        } catch {
            return replyHandler(error)
        }
        return replyHandler(nil)
    }

    /// Performs a `close` operation on the given file item.
    /// - Parameters:
    ///   - item: The file item to close.
    ///   - modes: The open modes (ignored for SocietyFS).
    ///   - replyHandler: The reply handler to invoke with the result.
    public func closeItem(_ item: FSItem,
                          modes: FSVolume.OpenModes,
                          replyHandler: @escaping ((any Error)?) -> Void) {
        guard let ptItem = item as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast item")
            return replyHandler(POSIXError(.EINVAL))
        }
        guard ptItem != self.rootItem else {
            // Root item is closed in deactivate volume.
            return replyHandler(nil)
        }

        do {
            try ptItem.closeItem()
        } catch {
            return replyHandler(error)
        }
        return replyHandler(nil)
    }

    /// Get maximum link count using `fpathconf`.
    public var maximumLinkCount: Int {
        return Int(fpathconf(self.rootItem.fileDescriptor, _PC_LINK_MAX))
    }

    /// Get maximum name length using `fpathconf`.
    public var maximumNameLength: Int {
        return Int(fpathconf(self.rootItem.fileDescriptor, _PC_NAME_MAX))
    }

    /// Get whether the volume restricts ownership changes based on authorization using `fpathconf`.
    public var restrictsOwnershipChanges: Bool {
        return fpathconf(self.rootItem.fileDescriptor, _PC_CHOWN_RESTRICTED) == 1
    }

    /// Get whether the volume truncates files longer than its maximum supported length using `fpathconf`.
    public var truncatesLongNames: Bool {
        return fpathconf(self.rootItem.fileDescriptor, _PC_NO_TRUNC) == 0
    }

    /// Get the maximum file size in bits using `fpathconf`.
    public var maximumFileSizeInBits: Int {
        return Int(fpathconf(self.rootItem.fileDescriptor, _PC_FILESIZEBITS))
    }

    /// Get the maximum extended attribute size in bits using `fpathconf`.
    public var maximumXattrSizeInBits: Int {
        return Int(fpathconf(self.rootItem.fileDescriptor, _PC_XATTR_SIZE_BITS))
    }
}
