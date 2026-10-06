#pragma once
#include "iiSocietyContainerExport.h"
#include <expected>
#include <filesystem>
#include <string>

namespace iiSocietyContainer {
/// Ordinary native directory preparation; independent of Qt and disk-image APIs.
class IISOCIETY_FILETREE_EXPORT DirectoryStorage final {
public:
    struct Location { std::filesystem::path path; bool created = false; };
    /// Prepare Society/ under an existing canonical parent, or reuse a drive root.
    /// The SocietyDrive adapter initializes and validates its existing manifest.
    static std::expected<Location, std::string> prepare(const std::filesystem::path& parent);
};
}
