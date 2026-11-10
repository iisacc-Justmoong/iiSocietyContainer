#pragma once
#include <FileTree.h>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace iiSocietyContainer {

// A rebuildable, metadata-only view. Keys are scoped to the selected container;
// revision is a SHA-256 of metadata, not a digest of file contents.
struct InventoryEntry {
    std::string section, path, name, key, revision, previewToken;
    std::uint64_t size = 0;
    std::int64_t modifiedMs = 0, modifiedTicks = 0;
    bool directory = false;
};

struct InventorySection {
    std::string key, directory;
    std::uint64_t fingerprint = 0;
    bool complete = false;
    FileTree::Entry tree;
};

class IISOCIETY_FILETREE_EXPORT InventoryIndex final {
public:
    template<class T> using Result = std::expected<T, std::string>;
    // The caller owns the container monitor lease and validates containerId.
    // Incomplete sections are unavailable to readers and never delete prior rows.
    static Result<bool> publish(const std::filesystem::path &root, const std::string &containerId,
                                const std::vector<InventorySection> &sections,
                                const std::function<bool()> &stillValid = {});
    // Returns a complete immediate-child snapshot. It is provisional and must
    // never authorize access without validating the actual selected file.
    static Result<std::vector<InventoryEntry>> children(const std::filesystem::path &root,
        const std::string &containerId, const std::filesystem::path &relativeDirectory);
    static Result<std::optional<InventoryEntry>> find(const std::filesystem::path &root,
        const std::string &containerId, const std::string &key);
    static std::filesystem::path path(const std::filesystem::path &root);
};
}
