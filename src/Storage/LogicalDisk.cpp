#include "LogicalDisk.h"
#include "LogicalDiskFormat.h"
#include <iiFileProviderStorage.h>
#include <algorithm>
#include <cctype>

namespace iiSocietyContainer {
bool LogicalDisk::validOwner(std::string_view application) { return detail::logicalOwnerValid(application); }
std::optional<std::string> LogicalDisk::owner(const std::filesystem::path &root) { return detail::logicalOwner(root); }
std::expected<DirectoryStorage::Location, std::string> LogicalDisk::prepare(
    const std::filesystem::path &root, std::string_view application) {
    namespace fs = std::filesystem;
    try {
        const auto sourceExtension = [](const fs::path &path) {
            auto suffix = path.extension().string();
            std::ranges::transform(suffix, suffix.begin(), [](unsigned char c) { return std::tolower(c); });
            return suffix == ".societycontainer" || suffix == ".sparsebundle";
        };
        if (!validOwner(application) || !root.is_absolute() || root.filename().empty()
            || sourceExtension(root))
            return std::unexpected("Choose an absolute logical disk path and a valid application identifier.");
        const auto parent = root.parent_path();
        if (!fs::is_directory(fs::symlink_status(parent)) || fs::canonical(parent) != parent)
            return std::unexpected("The logical disk parent must exist without redirected components.");
        for (auto current = parent; !current.empty();) {
            if (fs::exists(fs::symlink_status(current / ".society-drive.json"))
                || fs::exists(fs::symlink_status(current / detail::logicalMarker))
                || fs::exists(fs::symlink_status(current / ".society-disk.plist"))
                || sourceExtension(current))
                return std::unexpected("A logical disk cannot be nested inside another Society source.");
            const auto next = current.parent_path(); if (next == current) break; current = next;
        }
        std::error_code error; const bool created = iiFileProvider::native::createDirectory(root, error);
        if (error || !fs::is_directory(fs::symlink_status(root)) || fs::canonical(root) != root)
            return std::unexpected("The logical disk path is unavailable or redirected.");
        if (const auto existing = owner(root)) {
            if (*existing != application) return std::unexpected("The logical disk belongs to another application.");
            return DirectoryStorage::Location{root, created};
        }
        if (fs::directory_iterator(root) != fs::directory_iterator{})
            return std::unexpected("Logical disk creation preserves and rejects an existing nonempty directory.");
        try {
            iiFileProvider::native::File::create(root / detail::logicalMarker, std::string(detail::logicalMagic) + std::string(application) + '\n');
        } catch (const std::exception &) {
            if (owner(root) != std::optional(std::string(application))) throw;
        }
        return DirectoryStorage::Location{root, created};
    } catch (const std::exception &error) { return std::unexpected(error.what()); }
}
}
