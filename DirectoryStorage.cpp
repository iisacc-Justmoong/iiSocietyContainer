#include "DirectoryStorage.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace iiSocietyContainer {
namespace {
bool directDirectory(const std::filesystem::path& path)
{
#ifdef _WIN32
    auto prefix = path.root_path();
    for (const auto& part : path.relative_path()) {
        prefix /= part;
        const auto attributes = GetFileAttributesW(prefix.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
#endif
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_directory(status)) return false;
    const auto canonical = std::filesystem::canonical(path, error);
    return !error && canonical == path;
}
bool legacyImage(const std::filesystem::path& path)
{
    return std::filesystem::exists(path / ".society-disk.plist")
        || (std::filesystem::exists(path / "Info.plist") && std::filesystem::is_directory(path / "bands"));
}
}
std::expected<DirectoryStorage::Location, std::string> DirectoryStorage::prepare(const std::filesystem::path& parent)
{
    try {
        if (!parent.is_absolute() || !directDirectory(parent))
            return std::unexpected("Choose an existing absolute directory without redirected components.");
        if (legacyImage(parent)) return std::unexpected("Choose a normal directory outside the legacy disk image.");
        if (std::filesystem::exists(parent / ".society-drive.json")) return Location{parent, false};
        for (auto ancestor = parent.parent_path(); !ancestor.empty();) {
            if (std::filesystem::exists(ancestor / ".society-drive.json") || legacyImage(ancestor))
                return std::unexpected("A Society source cannot be inside another container.");
            const auto next = ancestor.parent_path();
            if (next == ancestor) break;
            ancestor = next;
        }
        const auto path = parent / "Society";
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error && error != std::errc::no_such_file_or_directory)
            return std::unexpected(error.message());
        if (std::filesystem::exists(status) && (!directDirectory(path) || legacyImage(path)))
            return std::unexpected("The Society directory conflicts with an existing or redirected entry.");
        error.clear();
        const auto created = std::filesystem::create_directory(path, error);
        if (error) return std::unexpected(error.message());
        return Location{path, created};
    } catch (const std::filesystem::filesystem_error& error) {
        return std::unexpected(error.what());
    }
}
}
