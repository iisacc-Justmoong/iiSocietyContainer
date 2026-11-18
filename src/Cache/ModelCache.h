#pragma once
#include "iiSocietyContainerExport.h"
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

namespace iiSocietyContainer {

// Resource identity is supplied by the caller; revision describes the
// prepared content and is not used as the logical resource identifier.
struct ModelCacheKey {
    std::string resource, revision, entry;
    friend bool operator==(const ModelCacheKey &, const ModelCacheKey &) = default;
};
struct ModelCacheEntry {
    std::filesystem::path path;
    bool hit = false;
};

// The owner must first validate the Society drive and supply a callback which
// rechecks its identity/readiness. Operations perform blocking file I/O and
// must run on an I/O worker. No model inference or resident memory is implied.
class IISOCIETY_FILETREE_EXPORT ModelCache final {
public:
    template<class T> using Result = std::expected<T, std::string>;
    using Builder = std::function<Result<void>(const std::filesystem::path &, std::stop_token)>;
    using Validator = std::function<bool(const std::filesystem::path &)>;
    using DriveValid = std::function<bool()>;

    ModelCache(std::filesystem::path containerRoot, std::string containerId,
               std::string applicationId, std::filesystem::path catalogRelative,
               DriveValid driveValid);
    Result<std::filesystem::path> catalog() const;
    Result<void> select(const ModelCacheKey &) const;
    Result<std::optional<ModelCacheKey>> primary() const;
    Result<ModelCacheEntry> ensure(const ModelCacheKey &, const Builder &, const Validator &,
                                   std::stop_token = {}) const;

private:
    std::filesystem::path root_, catalogRelative_;
    std::string containerId_, applicationId_;
    DriveValid driveValid_;
    Result<std::filesystem::path> directory(const std::filesystem::path &, bool create) const;
    Result<void> validate(const ModelCacheKey &) const;
};
}
