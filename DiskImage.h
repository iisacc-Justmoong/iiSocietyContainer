#pragma once
#include "iiSocietyContainerExport.h"
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace iiSocietyContainer {
struct DiskVolume {
    std::filesystem::path imagePath;
    std::filesystem::path mountPath;
    std::string device;
    std::string imageDevice;
};

/// Native disk-image lifecycle. Does not initialize or reinterpret a directory.
/// The image package owns a private data volume and a public Files volume.
class IISOCIETYCONTAINER_EXPORT DiskImage {
public:
    static bool supported();
    /// Create an owned sparse disk, or mount that same existing owned image.
    /// bytes == 0 uses available backing-store space (a ceiling, not a reservation).
    static std::expected<DiskVolume, std::string> create(
        const std::filesystem::path &location, std::uint64_t bytes = 0);
    /// Never creates a missing image. Unknown files/images are preserved and rejected.
    static std::expected<DiskVolume, std::string> mount(const std::filesystem::path &image);
    static std::optional<DiskVolume> mountedAt(const std::filesystem::path &root);
    /// Actual public volume root for a private native container; no arbitrary links.
    static std::optional<std::filesystem::path> filesRoot(const std::filesystem::path &root);
    /// Resolve a public volume entry back to its mounted private container.
    static std::optional<std::filesystem::path> containerRoot(const std::filesystem::path &entry);
    /// Normal eject only: open files cause failure, never a forced detach.
    static std::expected<void, std::string> detach(const std::filesystem::path &image);
};
}
