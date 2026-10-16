#include <iiFileProviderStorage.h>
#include "DirectoryStorage.h"
#include <algorithm>
#include <cctype>
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
    return path.extension() == ".sparsebundle"
        || std::filesystem::exists(path / ".society-disk.plist")
        || std::filesystem::exists(path / "Society.volume")
        || (std::filesystem::exists(path / "Info.plist") && std::filesystem::is_directory(path / "bands"));
}
bool packageName(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char value) { return std::tolower(value); });
    return extension == ".societycontainer";
}
}
bool DirectoryStorage::isPackageDirectory(const std::filesystem::path& path)
{
    try {
        return path.is_absolute() && packageName(path) && directDirectory(path) && !legacyImage(path);
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    }
}
std::expected<DirectoryStorage::Location, std::string> DirectoryStorage::prepare(const std::filesystem::path& parent)
{
    try {
        if (!parent.is_absolute() || !directDirectory(parent))
            return std::unexpected("Choose an existing absolute directory without redirected components.");
        if (legacyImage(parent)) return std::unexpected("Choose a normal directory outside the legacy disk image.");
        for (auto ancestor = parent.parent_path(); !ancestor.empty();) {
            if (packageName(ancestor) || std::filesystem::exists(ancestor / ".society-drive.json") || legacyImage(ancestor))
                return std::unexpected("A Society source cannot be inside another container.");
            const auto next = ancestor.parent_path();
            if (next == ancestor) break;
            ancestor = next;
        }
        if (packageName(parent)) return Location{parent, false};
        if (std::filesystem::exists(parent / ".society-drive.json"))
            return std::unexpected("An existing Society source must be a .societycontainer package.");
        const auto path = parent / "Society.societycontainer";
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error && error != std::errc::no_such_file_or_directory)
            return std::unexpected(error.message());
        if (std::filesystem::exists(status) && (!directDirectory(path) || legacyImage(path)))
            return std::unexpected("The Society.societycontainer package conflicts with an existing or redirected entry.");
        error.clear();
        const auto created = iiFileProvider::native::createDirectory(path, error);
        if (error) return std::unexpected(error.message());
        return Location{path, created};
    } catch (const std::filesystem::filesystem_error& error) {
        return std::unexpected(error.what());
    }
}
}
