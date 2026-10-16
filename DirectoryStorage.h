#pragma once
#include "iiSocietyContainerExport.h"
#include <expected>
#include <filesystem>
#include <string>

namespace iiSocietyContainer {
/// Native directory-package preparation; independent of Qt and disk-image APIs.
class IISOCIETY_FILETREE_EXPORT DirectoryStorage final {
public:
    struct Location { std::filesystem::path path; bool created = false; };
    /// An existing canonical .societycontainer directory without legacy disk markers.
    /// This structural check does not read or validate the SocietyDrive manifest.
    static bool isPackageDirectory(const std::filesystem::path& path);
    /// Prepare Society.societycontainer under a canonical parent, or reuse a package.
    /// The SocietyDrive adapter initializes and validates its existing manifest.
    static std::expected<Location, std::string> prepare(const std::filesystem::path& parent);
};
}
