#pragma once
#include "iiSocietyContainerExport.h"
#include "DirectoryStorage.h"
#include <optional>
#include <string_view>

namespace iiSocietyContainer {
// Persistent application-owned native storage. It uses the ordinary Society
// manifest/sections without requiring a .societycontainer package name.
class IISOCIETY_FILETREE_EXPORT LogicalDisk final {
public:
    static bool validOwner(std::string_view application);
    static std::optional<std::string> owner(const std::filesystem::path &root);
    // Only an empty/new directory or this same owner's logical disk is accepted.
    // The parent must exist. Existing user contents and redirects are preserved.
    static std::expected<DirectoryStorage::Location, std::string> prepare(
        const std::filesystem::path &root, std::string_view application);
};
}
