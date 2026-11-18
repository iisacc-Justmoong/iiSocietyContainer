#include "ModelCache.h"
#include <iiFileProviderNative.h>
#include <iiFileProviderStorage.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <stdio.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace iiSocietyContainer {
namespace {
namespace fs = std::filesystem;
using iiFileProvider::native::File;
using iiFileProvider::native::TemporaryDirectory;

bool bounded(const std::string &s, std::size_t max) {
    return !s.empty() && s.size() <= max && s.find('\0') == std::string::npos;
}
bool entryName(const std::string &s) {
    return bounded(s, 128) && std::isalnum(static_cast<unsigned char>(s.front())) &&
        std::isalnum(static_cast<unsigned char>(s.back())) &&
        std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        });
}
bool validRelative(const fs::path &p) {
    if (p.empty() || p.is_absolute() || p.has_root_name() || p.lexically_normal() != p) return false;
    for (const auto &part : p) {
        const auto s = part.generic_string();
        if (s.empty() || s == "." || s == ".." || s.find(':') != s.npos || s.find('\\') != s.npos || s.find('\0') != s.npos) return false;
    }
    return true;
}
bool directDirectory(const fs::path &p) {
    std::error_code e;
    const auto status = fs::symlink_status(p, e);
    if (e || !fs::is_directory(status) || fs::is_symlink(status)) return false;
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(p.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
    return fs::canonical(p, e) == p && !e;
}
bool safeTree(const fs::path &root) {
    if (!directDirectory(root)) return false;
    std::error_code e;
    std::size_t count = 0;
    fs::recursive_directory_iterator it(root, fs::directory_options::none, e), end;
    for (; !e && it != end; it.increment(e)) {
        if (++count > 10000 || it.depth() > 64) return false;
        const auto status = it->symlink_status(e);
        if (e || fs::is_symlink(status) || (!fs::is_regular_file(status) && !fs::is_directory(status))) return false;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
    }
    return !e;
}
std::string field(const std::string &value) { return std::to_string(value.size()) + ':' + value; }
bool nextField(const std::string &input, std::size_t &pos, std::string &out) {
    const auto colon = input.find(':', pos);
    if (colon == input.npos || colon == pos || colon - pos > 6) return false;
    std::size_t length = 0;
    const auto parsed = std::from_chars(input.data() + pos, input.data() + colon, length);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + colon || length > 4096 || length > input.size() - colon - 1) return false;
    pos = colon + 1; out = input.substr(pos, length); pos += length; return true;
}
// Native no-replace publication: a concurrent producer can never overwrite a
// prepared or user-owned package. Unsupported platforms fail closed.
bool publishDirectory(const fs::path &source, const fs::path &target) {
#ifdef _WIN32
    return MoveFileW(source.c_str(), target.c_str()) != 0;
#elif defined(__APPLE__)
    return renamex_np(source.c_str(), target.c_str(), RENAME_EXCL) == 0;
#elif defined(__linux__) && defined(SYS_renameat2)
    return syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), 1) == 0;
#else
    (void)source; (void)target; return false;
#endif
}
}

ModelCache::ModelCache(fs::path root, std::string container, std::string app, fs::path catalog, DriveValid valid)
    : root_(std::move(root)), catalogRelative_(std::move(catalog)), containerId_(std::move(container)),
      applicationId_(std::move(app)), driveValid_(std::move(valid)) {}

ModelCache::Result<void> ModelCache::validate(const ModelCacheKey &key) const {
    if (!bounded(containerId_, 256) || !bounded(applicationId_, 256) || !validRelative(catalogRelative_) ||
        !bounded(key.resource, 4096) || !bounded(key.revision, 1024) || !entryName(key.entry))
        return std::unexpected("Invalid model cache identity or entry");
    return {};
}
ModelCache::Result<fs::path> ModelCache::directory(const fs::path &relativePath, bool create) const {
    try {
        if (!root_.is_absolute() || root_.lexically_normal() != root_ || !directDirectory(root_) ||
            !driveValid_ || !driveValid_() || !validRelative(relativePath))
            return std::unexpected("The Society container is unavailable or redirected");
        auto current = root_;
        for (const auto &part : relativePath) {
            current /= part;
            std::error_code e;
            auto status = fs::symlink_status(current, e);
            if (e == std::errc::no_such_file_or_directory || (!e && !fs::exists(status))) {
                if (!create) return std::unexpected("Model cache directory does not exist");
                e.clear(); (void)iiFileProvider::native::createDirectory(current, e);
                if (e) return std::unexpected(e.message());
            } else if (e) return std::unexpected(e.message());
            if (!directDirectory(current)) return std::unexpected("Model cache directory is redirected or unavailable");
        }
        if (!driveValid_()) return std::unexpected("The Society container identity changed");
        return current;
    } catch (const std::exception &e) { return std::unexpected(e.what()); }
}
ModelCache::Result<fs::path> ModelCache::catalog() const {
    if (!bounded(containerId_, 256) || !bounded(applicationId_, 256) || !validRelative(catalogRelative_))
        return std::unexpected("Invalid model cache scope");
    return directory(fs::path(".society-runtime") / catalogRelative_, true);
}
ModelCache::Result<void> ModelCache::select(const ModelCacheKey &key) const {
    if (auto status = validate(key); !status) return status;
    auto location = directory(fs::path(".society-runtime/cache/model-cache-v1") /
                              iiFileProvider::native::sha256(applicationId_), true);
    if (!location) return std::unexpected(location.error());
    try {
        const auto path = *location / "primary";
        std::error_code e; const auto status = fs::symlink_status(path, e);
        if (e && e != std::errc::no_such_file_or_directory) return std::unexpected(e.message());
        if (!e && fs::exists(status) && !fs::is_regular_file(status)) return std::unexpected("Primary model record is redirected");
        if (!e && fs::exists(status)) {
            const auto previous = primary();
            if (!previous) return std::unexpected(previous.error());
        }
        const auto bytes = std::string("IISCACHE1\n") + field(containerId_) + field(applicationId_) +
            field(catalogRelative_.generic_string()) + field(key.resource) + field(key.revision) + field(key.entry);
        if (!driveValid_()) return std::unexpected("The Society container identity changed");
        File::write(path, bytes);
        return {};
    } catch (const std::exception &e) { return std::unexpected(e.what()); }
}
ModelCache::Result<std::optional<ModelCacheKey>> ModelCache::primary() const {
    if (!bounded(containerId_, 256) || !bounded(applicationId_, 256) || !validRelative(catalogRelative_))
        return std::unexpected("Invalid model cache scope");
    auto location = directory(fs::path(".society-runtime/cache/model-cache-v1") /
                              iiFileProvider::native::sha256(applicationId_), true);
    if (!location) return std::unexpected(location.error());
    try {
        const auto path = *location / "primary";
        std::error_code e; const auto status = fs::symlink_status(path, e);
        if (e == std::errc::no_such_file_or_directory || (!e && !fs::exists(status))) return std::optional<ModelCacheKey>{};
        if (e || !fs::is_regular_file(status) || fs::is_symlink(status)) return std::unexpected("Primary model record is redirected");
        const auto bytes = File::read(path, 16384);
        constexpr std::string_view magic = "IISCACHE1\n";
        if (!bytes.starts_with(magic)) return std::unexpected("Invalid primary model record");
        std::size_t pos = magic.size(); std::string container, app, catalog; ModelCacheKey key;
        if (!nextField(bytes,pos,container) || !nextField(bytes,pos,app) || !nextField(bytes,pos,catalog) ||
            !nextField(bytes,pos,key.resource) || !nextField(bytes,pos,key.revision) || !nextField(bytes,pos,key.entry) ||
            pos != bytes.size() || container != containerId_ || app != applicationId_ || catalog != catalogRelative_.generic_string())
            return std::unexpected("Primary model record has a different scope or is invalid");
        if (auto valid = validate(key); !valid) return std::unexpected(valid.error());
        if (!driveValid_()) return std::unexpected("The Society container identity changed");
        return std::optional<ModelCacheKey>{std::move(key)};
    } catch (const std::exception &e) { return std::unexpected(e.what()); }
}
ModelCache::Result<ModelCacheEntry> ModelCache::ensure(const ModelCacheKey &key, const Builder &build,
                                                        const Validator &verify, std::stop_token stop) const {
    if (auto status = validate(key); !status) return std::unexpected(status.error());
    if (!build || !verify || stop.stop_requested()) return std::unexpected("Model cache preparation was cancelled or is unavailable");
    auto location = catalog(); if (!location) return std::unexpected(location.error());
    const auto target = *location / key.entry;
    try {
        auto existing = [&]() -> Result<ModelCacheEntry> {
            if (!driveValid_() || !safeTree(target) || !verify(target))
                return std::unexpected("Cached model is invalid, redirected or no longer available");
            if (stop.stop_requested() || !driveValid_())
                return std::unexpected("Model cache preparation was cancelled or its container changed");
            return ModelCacheEntry{target, true};
        };
        std::error_code e; const auto status = fs::symlink_status(target, e);
        if (!e && fs::exists(status)) return existing();
        if (e && e != std::errc::no_such_file_or_directory) return std::unexpected(e.message());
        TemporaryDirectory staged(*location);
        if (!staged.isValid()) return std::unexpected("Cannot create private model staging directory");
        auto built = build(staged.path(), stop);
        if (!built) return std::unexpected(built.error());
        if (stop.stop_requested() || !driveValid_()) return std::unexpected("Model cache preparation was cancelled or its container changed");
        if (!safeTree(staged.path()) || !verify(staged.path())) return std::unexpected("Prepared model failed validation");
        if (stop.stop_requested() || !driveValid_()) return std::unexpected("Model cache preparation was cancelled or its container changed");
        if (publishDirectory(staged.path(), target)) { staged.release(); return ModelCacheEntry{target, false}; }
        if (fs::exists(fs::symlink_status(target))) return existing();
        return std::unexpected("Cannot publish the prepared model without replacement");
    } catch (const std::exception &e) { return std::unexpected(e.what()); }
}
}
