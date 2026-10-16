/*
See the LICENSE.txt file for this sample’s licensing information.

Abstract:
A class that defines a custom item for use by the society file system.
*/

import Foundation
import ExtensionFoundation
import FSKit
import OSLog

/// Defined item open modes.
enum SocietyFSItemOpenMode: Int32 {
    /// Close mode
    case close = -1
    /// Read-only mode
    case readOnly = 0
    /// Read and write mode
    case readWrite = 1
}

/// A SocietyFSItem represents a file system item.
class SocietyFSItem: FSItem {

    /// File descriptor of the item
    var fileDescriptor: Int32
    /// Open mode of the item
    var openMode: SocietyFSItemOpenMode
    /// Parent item (for a root item, the parent is nil)
    var parent: SocietyFSItem?
    /// Name of the item
    var name: String
    /// Item type
    var itemType: FSItem.ItemType
    /// Inode of the item (used to cache items in SocietyFSVolume)
    var inode: UInt64
    /// Dispatch queue for changing the item's SocietyFSItemOpenMode
    var openModeQueue: DispatchQueue
    /// FSKit attribute sequence numbers require one population context per item.
    let attributesQueue = DispatchQueue(label: "com.iisacc.societyfs.item.attributes.queue")

    /// OS services can use these mounted names as their own storage. Existing
    /// user entries and descendants are exposed read-only to preserve source data.
    var isReadOnlyMetadata: Bool {
        var item: SocietyFSItem? = self
        while let current = item, let parent = current.parent {
            if societyMetadataNameIsReserved(current.name, atVolumeRoot: parent.parent == nil) {
                return true
            }
            item = parent
        }
        return false
    }

    /// Creates a new instance of SocietyFSItem, to create the root item.
    /// - Parameters:
    ///   - name: The name of the item.
    ///   - fileDescriptor: The file descriptor of the item.
    ///   - type: The type of the item.
    ///   - openFlags: The open mode of the item.
    init(name: String, fileDescriptor: Int32, type: FSItem.ItemType, openFlags: SocietyFSItemOpenMode) {
        self.name           = name
        self.parent         = nil
        self.fileDescriptor = fileDescriptor
        self.itemType       = type
        self.openMode       = openFlags
        self.inode          = 0
        self.openModeQueue = DispatchQueue(label: "com.apple.fskit.societyfs.item.\(name).openmode.queue")
        super.init()
        try? self.initInode()
    }

    /// Creates a new instance of SocietyFSItem.
    /// - Parameters:
    ///   - name: The name of the item.
    ///   - parent: The parent item.
    ///   - type: The type of the item.
    init(name: String, parent: SocietyFSItem, type: FSItem.ItemType) throws {
        self.name           = name
        self.parent         = parent
        self.openMode       = .close
        self.fileDescriptor = -1
        self.itemType       = type
        self.inode          = 0
        self.openModeQueue  = DispatchQueue(label: "com.apple.fskit.societyfs.item.\(name).openmode.queue")
        super.init()
        try self.upgradeOpenMode(mode: .readOnly)
        try self.initInode()
        try self.closeItem()
    }

    /// Initializes the inode number of the item.
    private func initInode() throws {
        var statResult = stat()
        _ = try throwErrno { fstat(self.fileDescriptor, &statResult) }
        self.inode = statResult.st_ino
    }

    /// Opens the item with given mode using `openat`, and if successful, returns a new file descriptor, otherwise throwing errno.
    ///
    /// The parent descriptor is owned by this request, independent of cached
    /// parent opens and closes performed by other callbacks.
    private func openWithMode(mode: SocietyFSItemOpenMode) throws -> Int32 {
        guard let parent = self.parent else {
            Logger.societyfs.error("\(#function): The parent is nil, can't open the item (\(self.name))")
            throw POSIXError(.EBADF)
        }
        let parentFD = try parent.descriptorForIO(mode: .readOnly)
        defer { Darwin.close(parentFD) }

        // Convert SocietyFSItemOpenMode to O_RDONLY/O_RDWR.
        var convertedMode = O_RDONLY
        if mode == .readWrite {
            convertedMode = O_RDWR
        }
        // Open the file.
        return try throwErrno { openat(parentFD, self.name, convertedMode | O_SYMLINK | O_CLOEXEC) }
    }

    /// Borrows an operation-owned descriptor with the required I/O mode.
    /// A metadata callback or a later close cannot invalidate the returned fd.
    func descriptorForIO(mode: SocietyFSItemOpenMode) throws -> Int32 {
        guard mode != .close else { throw POSIXError(.EINVAL) }
        return try self.openModeQueue.sync {
            try societyDescriptorForIO(self.fileDescriptor, writable: mode == .readWrite) {
                try self.openWithMode(mode: mode)
            }
        }
    }

    /// Upgrades the open mode of the item to the given mode.
    ///
    /// Upgrade means that the item can go from .close to .readOnly to .readWrite.
    /// If the item was opened to a .readOnly and needs to upgrade to .readWrite, this method creates a new file descriptor for the .readWrite mode,
    /// and closes the old .readOnly file descriptor.
    func upgradeOpenMode(mode: SocietyFSItemOpenMode) throws {
        if mode == .close {
            Logger.societyfs.error("\(#function): Can't pass .close as mode")
            throw POSIXError(.EINVAL)
        }
        try self.openModeQueue.sync {
            if (self.fileDescriptor != -1) && (self.openMode == .readWrite || self.openMode == mode) {
                return
            }
            let oldFD = self.fileDescriptor
            try self.fileDescriptor = self.openWithMode(mode: mode)

            // With a new fileDescriptor set, close the old file descriptor if it exists.
            if oldFD >= 0 {
                _ = try throwErrno { Darwin.close(oldFD) }
            }
            self.openMode = mode
        }
    }

    /// Closes the item by calling `close`.
    func closeItem() throws {
        try self.openModeQueue.sync {
            if self.fileDescriptor == -1 || self.openMode == .close {
                return
            }
            _ = try throwErrno {
                self.openMode == .readWrite ? IisaccFileProvider.closeAfterWriting(self.fileDescriptor) : Darwin.close(self.fileDescriptor)
            }
            self.fileDescriptor = -1
            self.openMode = .close
        }
    }

}
