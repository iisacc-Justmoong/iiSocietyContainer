/*
See the LICENSE.txt file for this sample’s licensing information.

Abstract:
The implementation of the society file system's conformance to the operations required of an FSKit volume.
*/

import Foundation
import ExtensionFoundation
import FSKit
import OSLog

// Only an explicit removal of an otherwise empty public folder may remove its
// persistent lease sentinel. Background maintenance never removes this file.
private func removeEmptySocietyDirectory(parent: Int32, name: String) -> Int32 {
    let directory = IisaccFileProvider.openAt(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
    guard directory >= 0 else { return -1 }
    defer { let failure = errno; close(directory); errno = failure }
    let sentinel = ".iisacc-temporary.lock"
    let lease = IisaccFileProvider.openAt(directory, sentinel, O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)
    if lease < 0 {
        guard errno == ENOENT else { return -1 }
        return unlinkat(parent, name, AT_REMOVEDIR)
    }
    defer { let failure = errno; close(lease); errno = failure }
    while flock(lease, LOCK_EX | LOCK_NB) != 0 {
        if errno == EINTR { continue }
        if errno == EWOULDBLOCK || errno == EAGAIN { errno = EBUSY }
        return -1
    }
    defer { let failure = errno; flock(lease, LOCK_UN); errno = failure }
    var held = stat(), entry = stat(), openedDirectory = stat(), namedDirectory = stat()
    guard fstat(lease, &held) == 0,
          fstatat(directory, sentinel, &entry, AT_SYMLINK_NOFOLLOW) == 0,
          fstat(directory, &openedDirectory) == 0,
          fstatat(parent, name, &namedDirectory, AT_SYMLINK_NOFOLLOW) == 0 else { return -1 }
    guard (held.st_mode & S_IFMT) == S_IFREG, held.st_size == 0, held.st_nlink == 1,
          held.st_dev == entry.st_dev, held.st_ino == entry.st_ino,
          openedDirectory.st_dev == namedDirectory.st_dev,
          openedDirectory.st_ino == namedDirectory.st_ino else { errno = EBUSY; return -1 }
    let scan = dup(directory)
    guard scan >= 0 else { return -1 }
    guard let listing = fdopendir(scan) else { let failure = errno; close(scan); errno = failure; return -1 }
    defer { let failure = errno; closedir(listing); errno = failure }
    errno = 0
    while let child = readdir(listing) {
        let childName = withUnsafePointer(to: &child.pointee.d_name) {
            $0.withMemoryRebound(to: CChar.self, capacity: Int(child.pointee.d_namlen) + 1) { String(cString: $0) }
        }
        if childName != "." && childName != ".." && childName != sentinel { errno = ENOTEMPTY; return -1 }
    }
    guard errno == 0 else { return -1 }
    guard unlinkat(directory, sentinel, 0) == 0 else { return -1 }
    // A queued C++ writer rechecks its sentinel inode after acquiring Shared;
    // if the source folder disappeared it rejects before creating a payload.
    return unlinkat(parent, name, AT_REMOVEDIR)
}


/// A structure that holds common attributes for all items.
private struct CommonAttributes {
    var length: UInt32
    var backupTime: timespec
    var parentID: UInt64
    var addedTime: timespec

    init() {
        self.length = 0
        self.backupTime = timespec(tv_sec: 0, tv_nsec: 0)
        self.parentID = 0
        self.addedTime = timespec(tv_sec: 0, tv_nsec: 0)
    }
}

/// Implementing FSVolume.Operations.
extension SocietyFSVolume: FSVolume.Operations {

    /// Returns volume statistics using `fstatfs`.
    public var volumeStatistics: FSStatFSResult {
        var statfsResult = statfs()
        let res = FSStatFSResult(fileSystemTypeName: String("societyfs"))
        if fstatfs(self.rootItem.fileDescriptor, &statfsResult) == -1 {
            return res
        }
        // Convert the statfs result to FSStatFSResult
        res.blockSize           = Int(statfsResult.f_bsize)
        res.ioSize              = Int(statfsResult.f_iosize)
        res.totalBlocks         = UInt64(statfsResult.f_blocks)
        res.availableBlocks     = UInt64(statfsResult.f_bavail)
        res.freeBlocks          = UInt64(statfsResult.f_bfree)
        res.usedBlocks          = res.totalBlocks - res.freeBlocks
        res.totalFiles          = UInt64(statfsResult.f_files)
        res.freeFiles           = UInt64(statfsResult.f_ffree)
        res.fileSystemSubType   = Int(statfsResult.f_fssubtype)
        return res
    }

    /// Activates the volume, but SocietyFS volume activation doesn't need to do anything, so this method just replies with the root item.
    ///
    /// - Parameters
    ///   - options: The activation options.
    ///   - reply: The reply handler to invoke when the activation is complete.
    public func activate(options: FSTaskOptions,
                         replyHandler reply: @escaping (FSItem?, (any Error)?) -> Void) {
        return reply(self.rootItem, nil)
    }

    /// Deactivates the volume, by closing the root item.
    /// - Parameters:
    ///   - options: The deactivation options.
    ///   - replyHandler: The reply handler to invoke when the deactivation is complete.
    public func deactivate(options: FSDeactivateOptions = [],
                           replyHandler: @escaping ((any Error)?) -> Void) {
        try? self.rootItem.closeItem()
        return replyHandler(nil)
    }

    /// Mount in SocietyFSVolume doesn't need to do anything; implementation just replies with nil error.
    public func mount(options: FSTaskOptions,
                      replyHandler: @escaping (Error?) -> Void) {
        return replyHandler(nil)
    }

    /// Unmount closes the root item.
    public func unmount(replyHandler: @escaping () -> Void) {
        try? self.rootItem.closeItem()
        return replyHandler()
    }

    /// Performs synchronize using `fsync` on the root item, ignoring the FSSyncFlags flags, which SocietyFS doesn't support.
    /// - Parameters
    ///   - flags: The sync flags.
    ///   - reply: The reply handler to invoke when the sync is complete.
    public func synchronize(flags: FSSyncFlags,
                            replyHandler reply: @escaping ((any Error)?) -> Void) {
        guard fsync(self.rootItem.fileDescriptor) == 0 else {
            let err = posixErrno
            Logger.societyfs.error("\(#function): Failed to synchronize with error(\(err))")
            return reply(err)
        }
        return reply(nil)
    }

    private func getCommonAttributes(descriptor: Int32,
                                     desiredAttributes: FSItem.GetAttributesRequest) throws -> CommonAttributes {

        var attrgroupFlags: Int32 = 0
        if desiredAttributes.isAttributeWanted(.parentID) {
            attrgroupFlags |= ATTR_CMN_PARENTID
        }
        if desiredAttributes.isAttributeWanted(.addedTime) {
            attrgroupFlags |= ATTR_CMN_ADDEDTIME
        }
        if desiredAttributes.isAttributeWanted(.backupTime) {
            attrgroupFlags |= ATTR_CMN_BKUPTIME
        }
        let commonAttrsWanted = attrgroup_t(attrgroupFlags)
        var attrList = attrlist(bitmapcount: u_short(ATTR_BIT_MAP_COUNT), reserved: 0, commonattr: commonAttrsWanted,
                                volattr: 0, dirattr: 0, fileattr: 0, forkattr: 0)
        var commonAttrsBuf = CommonAttributes()

        if attrgroupFlags != 0 {
            if fgetattrlist(descriptor, &attrList, &commonAttrsBuf, MemoryLayout<CommonAttributes>.size, UInt32(FSOPT_NOFOLLOW)) == -1 {
                throw posixErrno
            }
        }
        return commonAttrsBuf
    }

    /// Fetches attributes for the given item.
    /// The method uses `stat`, and `fgetattrlist` to get the attributes.
    public func getAttributes(_ desiredAttributes: FSItem.GetAttributesRequest,
                              of item: FSItem,
                              replyHandler: @escaping (FSItem.Attributes?, Error?) -> Void) {
        guard let ptItem = item as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast item")
            return replyHandler(nil, POSIXError(.EINVAL))
        }

        let result = Result {
            try ptItem.attributesQueue.sync {
                try self.readAttributes(desiredAttributes, of: ptItem)
            }
        }
        switch result {
        case .success(let attributes): replyHandler(attributes, nil)
        case .failure(let error): replyHandler(nil, error)
        }
    }

    private func readAttributes(_ desiredAttributes: FSItem.GetAttributesRequest,
                                of ptItem: SocietyFSItem) throws -> FSItem.Attributes {
        let descriptor: Int32
        do {
            descriptor = try ptItem.descriptorForIO(mode: .readOnly)
        } catch {
            Logger.societyfs.error("\(#function): Can't open given item (\(ptItem.name)) error (\(error))")
            throw error
        }
        defer { Darwin.close(descriptor) }

        var statResult = stat()
        if fstat(descriptor, &statResult) == -1 {
            throw posixErrno
        }

        var commonAttrsBuf: CommonAttributes
        do {
            commonAttrsBuf = try self.getCommonAttributes(descriptor: descriptor, desiredAttributes: desiredAttributes)
        } catch {
            Logger.societyfs.error("\(#function): Can't get commont attributes for item (\(ptItem.name))")
            throw error
        }

        let attrs = FSItem.Attributes()

        if desiredAttributes.isAttributeWanted(.uid) {
            attrs.uid = statResult.st_uid
        }

        if desiredAttributes.isAttributeWanted(.gid) {
            attrs.gid = statResult.st_gid
        }

        if desiredAttributes.isAttributeWanted(.mode) {
            attrs.mode = UInt32(Int32(statResult.st_mode) & modeAllBits)
        }

        if desiredAttributes.isAttributeWanted(.linkCount) {
            attrs.linkCount = UInt32(statResult.st_nlink)
        }

        if desiredAttributes.isAttributeWanted(.flags) {
            attrs.flags = statResult.st_flags
        }

        if desiredAttributes.isAttributeWanted(.size) {
            attrs.size = UInt64(statResult.st_size)
        }

        if desiredAttributes.isAttributeWanted(.allocSize) {
            attrs.allocSize = societyAllocatedSize(statResult)
        }

        if desiredAttributes.isAttributeWanted(.fileID) {
            attrs.fileID = FSItem.Identifier(rawValue: statResult.st_ino) ?? .invalid
        }

        if desiredAttributes.isAttributeWanted(.parentID) {
            attrs.parentID = FSItem.Identifier(rawValue: commonAttrsBuf.parentID) ?? .invalid
        }

        if desiredAttributes.isAttributeWanted(.type) {
            attrs.type = ptItem.itemType
        }

        var timeSpec: timespec

        if desiredAttributes.isAttributeWanted(.accessTime) {
            timeSpec = statResult.st_atimespec
            attrs.accessTime = timeSpec
        }

        if desiredAttributes.isAttributeWanted(.changeTime) {
            timeSpec = statResult.st_ctimespec
            attrs.changeTime = timeSpec
        }

        if desiredAttributes.isAttributeWanted(.modifyTime) {
            timeSpec = statResult.st_mtimespec
            attrs.modifyTime = timeSpec
        }

        if desiredAttributes.isAttributeWanted(.addedTime) {
            timeSpec = commonAttrsBuf.addedTime
            attrs.addedTime = timeSpec
        }

        if desiredAttributes.isAttributeWanted(.birthTime) {
            timeSpec = statResult.st_birthtimespec
            attrs.birthTime = timeSpec
        }

        if desiredAttributes.isAttributeWanted(.backupTime) {
            timeSpec = commonAttrsBuf.backupTime
            attrs.backupTime = timeSpec
        }

        return attrs
    }

    /// Set item attributes.
    /// The method uses `ftruncate`, `fchmod`, `futimes`, `fchown`, and `fchflags`  to set the attributes.
    public func setAttributes(_ newAttributes: FSItem.SetAttributesRequest,
                              on item: FSItem,
                              creatingNewFile: Bool,
                              replyHandler: @escaping (FSItem.Attributes?, Error?) -> Void) {
        guard let ptItem = item as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast item")
            return replyHandler(nil, POSIXError(.EINVAL))
        }
        guard !ptItem.isReadOnlyMetadata else { return replyHandler(nil, POSIXError(.EPERM)) }

        // Check that this request doesn't attempt to change read-only fields, raising an error if it does.
        if (creatingNewFile == false) &&
            (newAttributes.isValid(.type) || newAttributes.isValid(.linkCount) ||
             newAttributes.isValid(.allocSize) || newAttributes.isValid(.fileID) ||
             newAttributes.isValid(.parentID) || newAttributes.isValid(.changeTime)) {
            return replyHandler(nil, POSIXError(.EINVAL))
        }

        if newAttributes.isValid(.mode) && ((Int32(newAttributes.mode) & ~modeAllBits) != 0) {
            // Bits outside of the supported mode bits are specified.
            Logger.societyfs.error("\(#function): Invalid mode bits for item (\(ptItem.name)), returning EINVAL")
            return replyHandler(nil, POSIXError(.EINVAL))
        }

        var getAttrs: FSItem.Attributes?
        var getAttrsError: Error?
        let getAttrRequest: FSItem.GetAttributesRequest = FSItem.GetAttributesRequest()
        getAttrRequest.wantedAttributes = [.gid, .uid, .mode, .size, .allocSize,
                                           .type, .fileID, .parentID, .flags,
                                           .linkCount, .accessTime, .birthTime,
                                           .modifyTime, .changeTime]

        if newAttributes.isValid(.accessTime) ||
            newAttributes.isValid(.modifyTime) ||
            newAttributes.isValid(.uid)        ||
            newAttributes.isValid(.gid) {
            self.getAttributes(getAttrRequest, of: item) { (attrs, error) in
                getAttrsError = error
                getAttrs = attrs
            }
            if getAttrsError != nil {
                return replyHandler(nil, getAttrsError)
            }
        }

        let oldItemFD = ptItem.fileDescriptor
        if oldItemFD == -1 {
            do {
                try ptItem.upgradeOpenMode(mode: .readOnly)
            } catch {
                Logger.societyfs.error("\(#function): Can't upgrade item (\(ptItem.name)) to set item attributes")
                return replyHandler(nil, error)
            }
        }

        if newAttributes.isValid(.size) {
            if ptItem.itemType != FSItem.ItemType.file {
                Logger.societyfs.error("\(#function): Can't change size of non file item (\(ptItem))")
                return replyHandler(nil, fs_errorForPOSIXError(EPERM))
            }
            do {
                try ptItem.upgradeOpenMode(mode: .readWrite)
            } catch {
                Logger.societyfs.error("\(#function): Can't upgrade item (\(ptItem.name)) to ftruncate")
                return replyHandler(nil, error)
            }
            if ftruncate(ptItem.fileDescriptor, Int64(newAttributes.size)) < 0 {
                replyHandler(nil, posixErrno)
                if oldItemFD == -1 {
                    try? ptItem.closeItem()
                }
                return
            }
        }

        if newAttributes.isValid(.mode) {
            let updatedMode = ((Int32(newAttributes.mode) & modeAllBits))
            if fchmod(ptItem.fileDescriptor, mode_t(updatedMode)) < 0 {
                return replyHandler(nil, posixErrno)
            }
        }

        if newAttributes.isValid(.accessTime) || newAttributes.isValid(.modifyTime) {
            let times = UnsafeMutablePointer<timeval>.allocate(capacity: 2)
            var accessTime = timespec(tv_sec: 0, tv_nsec: 0)
            var modifyTime = timespec(tv_sec: 0, tv_nsec: 0)

            if newAttributes.isValid(.accessTime) {
                accessTime = newAttributes.accessTime
            } else {
                accessTime = getAttrs!.accessTime
            }

            if newAttributes.isValid(.modifyTime) {
                modifyTime = newAttributes.modifyTime
            } else {
                modifyTime = getAttrs!.modifyTime
            }

            times[0] = timeval(tv_sec: accessTime.tv_sec, tv_usec: (__darwin_suseconds_t)(accessTime.tv_nsec / 1000))
            times[1] = timeval(tv_sec: modifyTime.tv_sec, tv_usec: (__darwin_suseconds_t)(modifyTime.tv_nsec / 1000))
            do {
                try ptItem.upgradeOpenMode(mode: .readWrite)
            } catch {
                Logger.societyfs.error("\(#function): Can't upgrade item (\(ptItem.name)) to set futimes")
                return replyHandler(nil, error)
            }
            if futimes(ptItem.fileDescriptor, times) < 0 {
                replyHandler(nil, posixErrno)
                if oldItemFD == -1 {
                    try? ptItem.closeItem()
                }
                return
            }
            times.deallocate()
        }

        if newAttributes.isValid(.flags) {
            let supportedBSDFlags = (UF_IMMUTABLE | UF_HIDDEN)
            if newAttributes.flags & ~UInt32(supportedBSDFlags) != 0 {
                Logger.societyfs.error("\(#function): invalid BSD flags (\(newAttributes.flags))")
                replyHandler(nil, fs_errorForPOSIXError(EINVAL))
                if oldItemFD == -1 {
                    try? ptItem.closeItem()
                }
                return
            }
            if fchflags(ptItem.fileDescriptor, newAttributes.flags) < 0 {
                replyHandler(nil, posixErrno)
                if oldItemFD == -1 {
                    try? ptItem.closeItem()
                }
                return
            }
        }

        // Change the owner attribute last, since doing so earlier may prevent changing other things.
        if newAttributes.isValid(.uid) || newAttributes.isValid(.gid) {
            var newUid: uid_t = 0
            var newGid: gid_t = 0

            if newAttributes.isValid(.uid) {
                newUid = newAttributes.uid
            } else {
                newUid = getAttrs!.uid
            }

            newGid = newAttributes.isValid(.gid) ? newAttributes.gid : getAttrs!.gid

            if fchown(ptItem.fileDescriptor, newUid, newGid) < 0 {
                replyHandler(nil, posixErrno)
                if oldItemFD == -1 {
                    try? ptItem.closeItem()
                }
                return
            }
        }

        self.getAttributes(getAttrRequest, of: item) { (attrs, error) in
            getAttrsError = error
            getAttrs = attrs
        }
        replyHandler(getAttrs, getAttrsError)

        if oldItemFD == -1 {
            try? ptItem.closeItem()
        }
    }

    public func setAttributes(_ newAttributes: FSItem.SetAttributesRequest,
                              on item: FSItem,
                              replyHandler: @escaping (FSItem.Attributes?, Error?) -> Void) {
        return self.setAttributes(newAttributes, on: item, creatingNewFile: false, replyHandler: replyHandler)
    }

    /// Performs a lookup on the given directory for the given name.
    /// Lookup is done by `fstatat`. If the item isn't in in the volume's item cache,  add it.
    /// - Parameters:
    ///   - name: The name of the item to lookup.
    ///   - directory: The directory to search.
    ///   - replyHandler: The handler to call when the lookup is complete.
    public func lookupItem(named name: FSFileName,
                           inDirectory directory: FSItem,
                           replyHandler: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        guard let dirItem = directory as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast directory")
            return replyHandler(nil, nil, POSIXError(.EINVAL))
        }

        guard let nameString = name.string else {
            Logger.societyfs.error("\(#function): Can't cast name to string")
            return replyHandler(nil, nil, POSIXError(.EINVAL))
        }

        guard !nameString.isEmpty, nameString != ".", nameString != "..",
              !nameString.contains("/"), !nameString.contains("\0") else {
            return replyHandler(nil, nil, POSIXError(.EINVAL))
        }
        guard !IisaccFileProvider.isControlFile(nameString) else { return replyHandler(nil, nil, POSIXError(.ENOENT)) }

        // Check if item exists in item cache.
        let type: FSItem.ItemType
        var statResult = stat()
        do {
            let descriptor = try dirItem.descriptorForIO(mode: .readOnly)
            defer { Darwin.close(descriptor) }
            _ = try throwErrno { fstatat(descriptor, nameString, &statResult, AT_SYMLINK_NOFOLLOW) }
            let inode = statResult.st_ino
            var val: SocietyFSItem?
            self.itemCacheQueue.sync {
                val = self.itemCache[inode]
            }
            if val != nil {
                return replyHandler(val, name, nil)
            }
            switch statResult.st_mode & S_IFMT {
            case S_IFDIR:
                type = FSItem.ItemType.directory
            case S_IFREG:
                type = FSItem.ItemType.file
            default:
                return replyHandler(nil, nil, POSIXError(.ENOENT))
            }
        } catch {
            return replyHandler(nil, nil, error)
        }

        // Item isn't in the item cache, create a new item, update the cache,  and return it.
        var newItem: SocietyFSItem
        do {
            newItem = try SocietyFSItem(name: nameString, parent: dirItem, type: type)
        } catch {
            Logger.societyfs.error("\(#function): Can't create new item (\(name.debugDescription)) error (\(error)")
            return replyHandler(nil, nil, error)
        }

        let retainedItem: SocietyFSItem = self.itemCacheQueue.sync {
            if let cached = self.itemCache[newItem.inode] { return cached }
            if newItem.inode != 0 { self.itemCache[newItem.inode] = newItem }
            return newItem
        }
        return replyHandler(retainedItem, name, nil)
    }

    /// Performs reclamation of an item, by removing the item from the item cache, and closing it.
    /// - Parameters:
    ///   - item: The item to be reclaimed.
    ///   - replyHandler: The reply handler to invoke.
    public func reclaimItem(_ item: FSItem, replyHandler: @escaping (Error?) -> Void) {
        guard let ptItem = item as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast item")
            return replyHandler(POSIXError(.EINVAL))
        }

        self.itemCacheQueue.sync {
            if self.itemCache[ptItem.inode] === ptItem {
                self.itemCache.removeValue(forKey: ptItem.inode)
            }
        }
        do {
            try ptItem.closeItem()
        } catch {
            return replyHandler(error)
        }
        return replyHandler(nil)
    }

    /// Reads a symbolic link, by calling `freadlink`.
    /// - Parameters
    ///   - item: The item to be read.
    ///   - replyHandler: The reply handler to invoke.
    public func readSymbolicLink(_ item: FSItem,
                                 replyHandler: @escaping (FSFileName?, Error?) -> Void) {
        guard let ptItem = item as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast item")
            return replyHandler(nil, POSIXError(.EINVAL))
        }
        let oldFD = ptItem.fileDescriptor
        if oldFD < 0 {
            try? ptItem.upgradeOpenMode(mode: .readOnly)
        }
        let buf = UnsafeMutablePointer<UTF8>.allocate(capacity: maxSymlinkSize)
        let bytesRead = freadlink(ptItem.fileDescriptor, buf, maxSymlinkSize)
        if bytesRead < 0 {
            buf.deallocate()
            replyHandler(nil, posixErrno)
            if oldFD < 0 {
                try? ptItem.upgradeOpenMode(mode: .close)
            }
            return
        }
        let data = Data(bytes: buf, count: bytesRead)
        buf.deallocate()
        replyHandler(FSFileName(data: data), nil)
        if oldFD < 0 {
            try? ptItem.upgradeOpenMode(mode: .close)
        }
    }

    /// Performs the creation of a new item in the specified directory, using `mkdirat` and `openat`.
    /// - Parameters:
    ///   - name: The name of the item to create.
    ///   - type: The type of the item to create.
    ///   - directory: The directory in which to create the item.
    ///   - newAttributes: The attributes of the new item.
    ///   - replyHandler: The reply handler to invoke.
    public func createItem(named name: FSFileName,
                           type: FSItem.ItemType,
                           inDirectory directory: FSItem,
                           attributes newAttributes: FSItem.SetAttributesRequest,
                           replyHandler: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        guard let dirItem = directory as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast dirItem")
            return replyHandler(nil, nil, POSIXError(.EINVAL))
        }

        guard let nameString = name.string else {
            Logger.societyfs.error("\(#function): Invalid name string (\(name.debugDescription))")
            return replyHandler(nil, nil, POSIXError(.EINVAL))
        }
        guard !IisaccFileProvider.isControlFile(nameString), !dirItem.isReadOnlyMetadata else {
            return replyHandler(nil, nil, POSIXError(.EPERM))
        }

        if (type == .file || type == .symlink) && !newAttributes.isValid(.mode) {
            Logger.societyfs.error("\(#function): attributes doesn't contain a valid mode.")
            return replyHandler(nil, nil, POSIXError(.EINVAL))
        }

        let oldDirItemFD = dirItem.fileDescriptor
        if oldDirItemFD < 0 {
            try? dirItem.upgradeOpenMode(mode: .readOnly)
        }

        do {
            if try societyExistingMetadataDestination(directory: dirItem.fileDescriptor, name: nameString,
                                                       atVolumeRoot: dirItem === self.rootItem) {
                if oldDirItemFD < 0 { try? dirItem.closeItem() }
                return replyHandler(nil, nil, POSIXError(.EEXIST))
            }
        } catch {
            if oldDirItemFD < 0 { try? dirItem.closeItem() }
            return replyHandler(nil, nil, error)
        }

        var newItem: SocietyFSItem
        var error: Int32 = -1
        var fileDescriptor: Int32 = -1
        nameString.withCString({ namePtr in
            switch type {
            case FSItem.ItemType.directory:
                error = IisaccFileProvider.makeDirectoryAt(dirItem.fileDescriptor, namePtr, S_IRWXU)
            case FSItem.ItemType.file:
                let createFlags = O_RDWR | O_CREAT | O_NOFOLLOW | O_SYMLINK | O_EXCL
                fileDescriptor = IisaccFileProvider.openAt(dirItem.fileDescriptor, namePtr, createFlags, S_IRWXU)
                if fileDescriptor >= 0 {
                    // Closing the fd, as we're about to open the file again when creating the item.
                    // (that way we have the same flow for files and dirs).
                    error = IisaccFileProvider.closeAfterWriting(fileDescriptor)
                } else {
                    error = -1
                }
            default:
                error = -1
                errno = EINVAL
            }
        })
        guard error != -1 else {
            return replyHandler(nil, nil, posixErrno)
        }

        if oldDirItemFD < 0 {
            try? dirItem.closeItem()
        }

        do {
            try newItem = SocietyFSItem(name: nameString, parent: dirItem, type: type)
        } catch {
            return replyHandler(nil, nil, error)
        }

        self.setAttributes(newAttributes, on: newItem, creatingNewFile: true, replyHandler: { (attrs, error) -> Void in
            guard error == nil else {
                return replyHandler(nil, nil, error)
            }
            self.itemCacheQueue.sync {
                self.itemCache[newItem.inode] = newItem
            }
            return replyHandler(newItem, name, error)
        })
    }

    /// Creates a new symbolic link using `symlinkat`.
    /// - Parameters:
    ///   - name: The name of the file to create.
    ///   - directory: The directory in which to create the symbolic link.
    ///   - newAttributes: The attributes to set on the newly created item.
    ///   - contents: The contents of the symbolic link.
    ///   - replyHandler: The handler to invoke when the operation completes.
    public func createSymbolicLink(named name: FSFileName,
                                   inDirectory directory: FSItem,
                                   attributes newAttributes: FSItem.SetAttributesRequest,
                                   linkContents contents: FSFileName,
                                   replyHandler: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        return replyHandler(nil, nil, POSIXError(.ENOTSUP))
    }

    /// Creation of hard links aren't support for SocietyFS.
    public func createLink(to item: FSItem,
                           named name: FSFileName,
                           inDirectory directory: FSItem,
                           replyHandler: @escaping (FSFileName?, Error?) -> Void) {
        return replyHandler(nil, POSIXError(.ENOTSUP))
    }

    /// Performs the actual removal of the given item from the given directory.
    /// - Parameters:
    ///   - item: The item to remove.
    ///   - name: The name of the item to remove.
    ///   - directory: The directory in which the item should be removed.
    ///   - replyHandler: The handler to call when the removal is complete.
    public func removeItem(_ item: FSItem,
                           named name: FSFileName,
                           fromDirectory directory: FSItem,
                           replyHandler: @escaping (Error?) -> Void) {
        guard let dirItem = directory as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast dirItem")
            return replyHandler(POSIXError(.EINVAL))
        }
        guard let ptItem = item as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast item")
            return replyHandler(POSIXError(.EINVAL))
        }
        guard let nameString = name.string else {
            Logger.societyfs.error("\(#function): Invalid name given")
            return replyHandler(POSIXError(.EINVAL))
        }
        guard !IisaccFileProvider.isControlFile(nameString), !ptItem.isReadOnlyMetadata,
              !societyMetadataMutationIsBlocked(name: nameString, atVolumeRoot: dirItem === self.rootItem,
                                                insideProtectedDirectory: dirItem.isReadOnlyMetadata) else {
            return replyHandler(POSIXError(.EPERM))
        }

        let unlinkFlags = (ptItem.itemType == FSItem.ItemType.directory) ? AT_REMOVEDIR : 0
        var error: Int32 = -1

        let oldDirItemFD = dirItem.fileDescriptor
        if oldDirItemFD < 0 {
            try? dirItem.upgradeOpenMode(mode: .readOnly)
        }
        nameString.withCString({ namePtr in
            error = unlinkFlags == AT_REMOVEDIR
                ? removeEmptySocietyDirectory(parent: dirItem.fileDescriptor, name: nameString)
                : unlinkat(dirItem.fileDescriptor, namePtr, unlinkFlags)
        })
        if error == -1 {
            return replyHandler(posixErrno)
        }

        // Remove from item cache the item.
        if ptItem.inode != 0 {
            _ = self.itemCacheQueue.sync {
                self.itemCache.removeValue(forKey: ptItem.inode)
            }
        }
        replyHandler(nil)

        // Close the directory, since it was closed when the call entered this method.
        if oldDirItemFD < 0 {
            try? dirItem.closeItem()
        }
    }

    /// Performs a rename operation on a file system item.
    /// - Parameters:
    ///   - item: The file system item to rename.
    ///   - sourceDirectory: The directory containing the source file system item.
    ///   - sourceName: The name of the item to rename.
    ///   - destinationName: The name of the destination file system item.
    ///   - destinationDirectory: The directory to move the item into.
    ///   - overItem: The item that should be overwritten if it already exists.
    ///   - replyHandler: The reply handler to call when the operation is complete.
    public func renameItem(_ item: FSItem,
                           inDirectory sourceDirectory: FSItem,
                           named sourceName: FSFileName,
                           to destinationName: FSFileName,
                           inDirectory destinationDirectory: FSItem,
                           overItem: FSItem?,
                           replyHandler: @escaping (FSFileName?, Error?) -> Void) {
        guard let fromItem = item as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast sourceName")
            return replyHandler(nil, POSIXError(.EINVAL))
        }
        guard let fromDir = sourceDirectory as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast sourceDirectory")
            return replyHandler(nil, POSIXError(.EINVAL))
        }
        guard let toDir = destinationDirectory as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast destinationDirectory")
            return replyHandler(nil, POSIXError(.EINVAL))
        }
        guard let publicSourceName = sourceName.string, let publicDestinationName = destinationName.string,
              !IisaccFileProvider.isControlFile(publicSourceName), !IisaccFileProvider.isControlFile(publicDestinationName) else {
            return replyHandler(nil, POSIXError(.EPERM))
        }
        guard !fromItem.isReadOnlyMetadata,
              !societyMetadataMutationIsBlocked(name: publicSourceName, atVolumeRoot: fromDir === self.rootItem,
                                                insideProtectedDirectory: fromDir.isReadOnlyMetadata),
              !societyMetadataMutationIsBlocked(name: publicDestinationName, atVolumeRoot: toDir === self.rootItem,
                                                insideProtectedDirectory: toDir.isReadOnlyMetadata) else {
            return replyHandler(nil, POSIXError(.EPERM))
        }

        let fromItemInode = fromItem.inode

        var toItemInode: UInt64 = 0
        if overItem != nil {
            guard let toItem = overItem as? SocietyFSItem else {
                Logger.societyfs.error("\(#function): Can't cast toItem")
                return replyHandler(nil, POSIXError(.EINVAL))
            }
            toItemInode = toItem.inode
        }
        // A previous read or enumeration may have closed either directory.
        // Reopen both parents before renameatx_np, then restore their state.
        let oldSourceFD = fromDir.fileDescriptor
        let oldDestinationFD = toDir.fileDescriptor
        do {
            try fromDir.upgradeOpenMode(mode: .readOnly)
            try toDir.upgradeOpenMode(mode: .readOnly)
        } catch {
            if oldSourceFD < 0 { try? fromDir.closeItem() }
            return replyHandler(nil, error)
        }
        defer {
            if oldSourceFD < 0 { try? fromDir.closeItem() }
            if oldDestinationFD < 0 && toDir !== fromDir { try? toDir.closeItem() }
        }
        do {
            _ = try societyExistingMetadataDestination(directory: toDir.fileDescriptor, name: publicDestinationName,
                                                       atVolumeRoot: toDir === self.rootItem)
        } catch {
            return replyHandler(nil, error)
        }
        // Perform rename call.
        var retVal: Int32 = -1
        let sourceString = sourceName.string
        let destinationString = destinationName.string
        sourceString!.withCString { fromName in
            destinationString!.withCString { toName in
                retVal = renameatx_np(fromDir.fileDescriptor, fromName, toDir.fileDescriptor, toName, 0)
            }
        }

        if retVal == -1 {
            Logger.societyfs.error("renameItem failed: errno \(errno), source FD \(fromDir.fileDescriptor), destination FD \(toDir.fileDescriptor)")
            return replyHandler(nil, posixErrno)
        }

        // Update fromItem.
        fromItem.name = destinationString!
        fromItem.parent = toDir
        if fromItem.itemType == .file, let descriptor = try? fromItem.descriptorForIO(mode: .readOnly) {
            IisaccFileProvider.closeAfterWriting(descriptor)
        }
        // Update itemCache
        self.itemCacheQueue.sync {
            // Remove old inode.
            self.itemCache.removeValue(forKey: fromItemInode)
            // Add new inode.
            self.itemCache[fromItem.inode] = fromItem
        }

        // If overItem did exist, remove it from the item cache
        if overItem != nil && overItem != fromItem {
            if toItemInode != 0 {
                _ = self.itemCacheQueue.sync {
                    self.itemCache.removeValue(forKey: toItemInode)
                }
            }
        }
        return replyHandler(destinationName, nil)
    }

    /// Performs an enumeration of the contents of a directory.
    /// - Parameters:
    ///   - directory: The directory to enumerate.
    ///   - cookie: The cookie returned by a previous call to enumerateDirectory().
    ///   - verifier: The directory verifier.
    ///   - attributes: The attributes to request for each item in the directory.
    ///   - packer: The packer to use to serialize directory entries.
    ///   - replyHandler: The handler to call when the enumeration is complete.
    public func enumerateDirectory(_ directory: FSItem,
                                   startingAt cookie: FSDirectoryCookie,
                                   verifier: FSDirectoryVerifier,
                                   attributes: FSItem.GetAttributesRequest?,
                                   packer: FSDirectoryEntryPacker,
                                   replyHandler: @escaping (FSDirectoryVerifier, Error?) -> Void) {
        guard let dirItem = directory as? SocietyFSItem else {
            Logger.societyfs.error("\(#function): Can't cast directory")
            return replyHandler(FSDirectoryVerifier(0), POSIXError(.EINVAL))
        }

        if dirItem.itemType != .directory {
            Logger.societyfs.error("\(#function): given item isn't a directory")
            return replyHandler(FSDirectoryVerifier(0), fs_errorForPOSIXError(ENOTDIR))
        }

        do {
            let directoryDescriptor = try dirItem.descriptorForIO(mode: .readOnly)
            defer { Darwin.close(directoryDescriptor) }
            let listing = try SocietyFSDirectoryStream(directory: directoryDescriptor, startingAt: cookie.rawValue)
            defer { listing.close() }

            while let entry = try listing.next() {
                let filename = withUnsafePointer(to: &entry.pointee.d_name) { namePtr -> String in
                    namePtr.withMemoryRebound(to: CChar.self, capacity: Int(entry.pointee.d_namlen) + 1) {
                        String(cString: $0)
                    }
                }
                let type = entry.pointee.d_type
                if filename == "." || filename == ".." || IisaccFileProvider.isControlFile(filename) ||
                    (type != DT_DIR && type != DT_REG) {
                    continue
                }

                let fsItemType: FSItem.ItemType = type == DT_DIR ? .directory : .file
                let itemID = entry.pointee.d_ino
                var itemAttributes: FSItem.Attributes? = nil
                if let attributes {
                    var itemError: Error?
                    self.lookupItem(named: FSFileName(string: filename), inDirectory: dirItem) { lookupItem, itemName, error in
                        guard let lookupItem, error == nil else {
                            itemError = error ?? POSIXError(.EIO)
                            return
                        }
                        self.getAttributes(attributes, of: lookupItem) { innerItemAttributes, innerError in
                            itemAttributes = innerItemAttributes
                            itemError = innerError
                        }
                    }
                    if let itemError {
                        Logger.societyfs.error("\(#function): Can't fetch enumeration attributes for \(filename): \(itemError)")
                        return replyHandler(FSDirectoryVerifier(0), itemError)
                    }
                    guard itemAttributes != nil else { return replyHandler(FSDirectoryVerifier(0), POSIXError(.EIO)) }
                }
                let packed = packer.packEntry(name: FSFileName(string: filename),
                                              itemType: fsItemType,
                                              itemID: FSItem.Identifier(rawValue: itemID) ?? FSItem.Identifier.invalid,
                                              nextCookie: FSDirectoryCookie(listing.nextCookie),
                                              attributes: itemAttributes)
                if !packed { break }
            }
            return replyHandler(FSDirectoryVerifier(0), nil)
        } catch {
            return replyHandler(FSDirectoryVerifier(0), error)
        }
    }

    /// Returns `true` if the volume supports the specified capability, otherwise returns `false`.
    /// - Parameter capability: The capability to check.
    private func volumeSupportsCapability(capability: Int32) -> Bool {
        var attrs = attrlist()
        attrs.bitmapcount = UInt16(ATTR_BIT_MAP_COUNT)
        attrs.volattr = UInt32(ATTR_VOL_CAPABILITIES)

        let lenSize = MemoryLayout<UInt32>.size
        let size = lenSize + MemoryLayout<vol_capabilities_attr_t>.size
        let buffer = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 4)
        defer { buffer.deallocate() }
        if fgetattrlist(self.rootItem.fileDescriptor, &attrs, buffer, size, 0) == -1 {
            return false
        }

        let attrPtr = (buffer + lenSize).assumingMemoryBound(to: vol_capabilities_attr_t.self)
        let validClone = (attrPtr.pointee.valid.1 & UInt32(capability)) != 0
        let capClone = (attrPtr.pointee.capabilities.1 & UInt32(capability)) != 0
        return validClone && capClone
    }

    /// The set of volume capabilities supported by this instance.
    public var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let capabilities = FSVolume.SupportedCapabilities()
        capabilities.supportsSymbolicLinks                  = self.volumeSupportsCapability(capability: VOL_CAP_FMT_SYMBOLICLINKS)
        capabilities.supportsHardLinks                      = self.volumeSupportsCapability(capability: VOL_CAP_FMT_HARDLINKS)
        capabilities.supportsHiddenFiles                    = self.volumeSupportsCapability(capability: VOL_CAP_FMT_HIDDEN_FILES)
        capabilities.supportsPersistentObjectIDs            = self.volumeSupportsCapability(capability: VOL_CAP_FMT_PERSISTENTOBJECTIDS)
        capabilities.supportsJournal                        = self.volumeSupportsCapability(capability: VOL_CAP_FMT_JOURNAL)
        capabilities.supportsActiveJournal                  = self.volumeSupportsCapability(capability: VOL_CAP_FMT_JOURNAL_ACTIVE)
        capabilities.supportsSparseFiles                    = self.volumeSupportsCapability(capability: VOL_CAP_FMT_SPARSE_FILES)
        capabilities.supportsZeroRuns                       = self.volumeSupportsCapability(capability: VOL_CAP_FMT_ZERO_RUNS)
        capabilities.supportsFastStatFS                     = self.volumeSupportsCapability(capability: VOL_CAP_FMT_FAST_STATFS)
        capabilities.supports2TBFiles                       = self.volumeSupportsCapability(capability: VOL_CAP_FMT_2TB_FILESIZE)
        capabilities.supportsOpenDenyModes                  = self.volumeSupportsCapability(capability: VOL_CAP_FMT_OPENDENYMODES)
        capabilities.supports64BitObjectIDs                 = self.volumeSupportsCapability(capability: VOL_CAP_FMT_64BIT_OBJECT_IDS)
        capabilities.supportsDocumentID                     = self.volumeSupportsCapability(capability: VOL_CAP_FMT_DOCUMENT_ID)
        capabilities.supportsSharedSpace                    = self.volumeSupportsCapability(capability: VOL_CAP_FMT_SHARED_SPACE)
        capabilities.supportsVolumeGroups                   = self.volumeSupportsCapability(capability: VOL_CAP_FMT_VOL_GROUPS)
        capabilities.doesNotSupportVolumeSizes              = self.volumeSupportsCapability(capability: VOL_CAP_FMT_NO_VOLUME_SIZES)
        capabilities.doesNotSupportImmutableFiles           = self.volumeSupportsCapability(capability: VOL_CAP_FMT_NO_IMMUTABLE_FILES)
        capabilities.doesNotSupportRootTimes                = self.volumeSupportsCapability(capability: VOL_CAP_FMT_NO_ROOT_TIMES)
        capabilities.doesNotSupportSettingFilePermissions   = self.volumeSupportsCapability(capability: VOL_CAP_FMT_NO_PERMISSIONS)
        // Determine caseSensitivity:
        if self.volumeSupportsCapability(capability: VOL_CAP_FMT_CASE_SENSITIVE) {
            capabilities.caseFormat = FSVolume.CaseFormat.sensitive
        } else if self.volumeSupportsCapability(capability: VOL_CAP_FMT_CASE_PRESERVING) {
            capabilities.caseFormat = FSVolume.CaseFormat.insensitiveCasePreserving
        } else {
            capabilities.caseFormat = FSVolume.CaseFormat.insensitive
        }
        return capabilities
    }

}
