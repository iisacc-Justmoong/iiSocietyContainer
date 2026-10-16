#pragma once

#include "iiSocietyContainerExport.h"
#include <iiFileProviderPresentation.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

namespace iiSocietyContainer {

/// Native filesystem traversal with no Qt dependency. Entry/list/snapshot calls
/// read metadata only; presentation() explicitly reads a bounded provider preview.
/// Traversal does not follow redirected entries.
class IISOCIETY_FILETREE_EXPORT FileTree final {
public:
    enum class Kind { File, Directory };
    struct IISOCIETY_FILETREE_EXPORT Entry {
        std::filesystem::path path;
        std::filesystem::path relativePath;
        std::filesystem::path parentPath;
        Kind kind = Kind::File;
        std::uintmax_t size = 0; // Regular file bytes; directories are zero.
        std::filesystem::file_time_type lastModified;
        std::filesystem::perms permissions = std::filesystem::perms::unknown;
        bool childrenLoaded = false;
        std::vector<Entry> children;

        /// Finds a node by its root-relative path in this loaded snapshot.
        [[nodiscard]] const Entry* find(const std::filesystem::path& relative) const;
    };
    struct Options {
        bool includeHidden = false; // Dot-prefixed names.
        std::size_t maxDepth = 64; // Zero loads only the requested node.
        std::size_t maxEntries = 100000; // Includes the requested root node.
    };
    template<class T> struct Result {
        std::optional<T> value;
        std::error_code error;
        explicit operator bool() const { return value.has_value() && !error; }
    };
    using EntryResult = Result<Entry>;
    using ChildrenResult = Result<std::vector<Entry>>;
    using PresentationResult = Result<iiFileProvider::native::PresentationRecord>;

    /// Pins the canonical native directory. Invalid roots remain invalid.
    explicit FileTree(const std::filesystem::path& root);
    [[nodiscard]] const std::filesystem::path& rootPath() const;
    [[nodiscard]] bool isValid() const;
    /// Empty and "." select the root. Other paths must be strict relative paths.
    [[nodiscard]] EntryResult entry(const std::filesystem::path& relative = {}) const;
    /// On-demand provider details and bounded preview for an existing regular
    /// file. Traversal/listing remain payload-free. Rejects control/redirect paths.
    [[nodiscard]] PresentationResult presentation(const std::filesystem::path& relative) const;
    [[nodiscard]] ChildrenResult children(const std::filesystem::path& relative = {},
                                          bool includeHidden = false) const;
    [[nodiscard]] EntryResult snapshot(const std::filesystem::path& relative,
                                       const Options& options) const;
    [[nodiscard]] EntryResult snapshot(const std::filesystem::path& relative = {}) const;

private:
    [[nodiscard]] ChildrenResult readChildren(const std::filesystem::path& relative,
                                              bool includeHidden, std::size_t limit) const;
    std::filesystem::path m_root;
    std::error_code m_error;
};

} // namespace iiSocietyContainer
