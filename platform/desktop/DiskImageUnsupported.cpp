#include "DiskImage.h"
namespace iiSocietyContainer {
bool DiskImage::supported() { return false; }
std::expected<DiskVolume, std::string> DiskImage::create(const std::filesystem::path &, std::uint64_t) {
    return std::unexpected("Native disk images are not available on this platform.");
}
std::expected<DiskVolume, std::string> DiskImage::mount(const std::filesystem::path &) {
    return std::unexpected("This Society disk image requires macOS.");
}
std::optional<DiskVolume> DiskImage::mountedAt(const std::filesystem::path &) { return {}; }
std::optional<std::filesystem::path> DiskImage::filesRoot(const std::filesystem::path &) { return {}; }
std::optional<std::filesystem::path> DiskImage::containerRoot(const std::filesystem::path &) { return {}; }
std::expected<void, std::string> DiskImage::detach(const std::filesystem::path &) {
    return std::unexpected("This Society disk image requires macOS.");
}
}
