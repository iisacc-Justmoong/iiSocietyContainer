#pragma once
#include <iiFileProviderNative.h>
#include <filesystem>
#include <optional>
#include <string_view>

namespace iiSocietyContainer::detail {
inline constexpr auto logicalMarker = ".society-logical-disk";
inline constexpr std::string_view logicalMagic = "iisacc.logical-disk.v1\n";
inline bool logicalOwnerValid(std::string_view value) {
    if (value.empty() || value.size() > 128 || value.front() == '.' || value.back() == '.'
        || value.find("..") != value.npos || value.find('.') == value.npos) return false;
    for (const unsigned char c : value)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '-' && c != '.') return false;
    return true;
}
inline std::optional<std::string> logicalOwner(const std::filesystem::path &root) {
    namespace fs = std::filesystem;
    try {
        if (!root.is_absolute() || !fs::is_directory(fs::symlink_status(root)) || fs::canonical(root) != root) return {};
        for (const auto *name : {".society-disk.plist", "Society.volume", "bands"})
            if (fs::exists(fs::symlink_status(root / name))) return {};
        const auto path = root / logicalMarker;
        if (!fs::is_regular_file(fs::symlink_status(path)) || fs::canonical(path) != path) return {};
        iiFileProvider::native::ReadFile file(path);
        if (file.size() > logicalMagic.size() + 129) return {};
        const auto bytes = file.read(static_cast<std::int64_t>(file.size()));
        if (!bytes.starts_with(logicalMagic) || bytes.empty() || bytes.back() != '\n') return {};
        const auto value = bytes.substr(logicalMagic.size(), bytes.size() - logicalMagic.size() - 1);
        return logicalOwnerValid(value) ? std::optional(value) : std::nullopt;
    } catch (const std::exception &) { return {}; }
}
}
