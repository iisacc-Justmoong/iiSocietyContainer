import Darwin

/// Allocated file bytes from the host filesystem's stat record.
func societyAllocatedSize(_ status: stat) -> UInt64 {
    // Darwin stat(2): st_blocks counts 512-byte sectors; st_blksize is the
    // preferred I/O block size and does not describe those sectors.
    UInt64(status.st_blocks) * 512
}
