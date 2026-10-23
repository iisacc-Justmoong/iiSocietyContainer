#pragma once
#include "iiSocietyContainerExport.h"
#include <OntologyFileSystem.h>
#include <functional>

namespace iiSocietyContainer {
struct FileRepositorySection { std::string key; std::filesystem::path directory; };
struct FileRepositoryContext {
    iiFileProvider::ObjectAuthor author;
    std::string device, description = "Society file operation";
    iiFileProvider::ObjectSessionContext work;
};
struct FileReconciliation { std::size_t files = 0, changed = 0, deleted = 0; };

/// Native file ownership index. Public files stay ordinary files; immutable RAW
/// originals, revisions, work sessions and semantic facts live inside the drive.
/// Blocking worker-thread API. One owner per thread; independent owners use WAL.
class IISOCIETY_FILETREE_EXPORT FileRepository final {
public:
    using Access = iiFileProvider::ObjectStore::Access;
    FileRepository(std::filesystem::path root, std::string container,
        std::vector<FileRepositorySection> sections, FileRepositoryContext context = {},
        Access access = Access::ReadWrite, std::function<bool()> available = {});
    ~FileRepository();
    FileRepository(const FileRepository &) = delete;
    FileRepository &operator=(const FileRepository &) = delete;

    /// Capture before overwriting, then observe again after complete publication.
    /// Unchanged files reuse their head. Metadata companions enter the same RAW
    /// annotation journal and indexed semantic projection.
    iiFileProvider::ObjectRecord observe(const std::string &path,
        const std::optional<iiFileProvider::FileMetadata> &metadata = {}, std::stop_token stop = {});
    FileReconciliation reconcile(std::stop_token stop = {});
    void captureTree(const std::string &path, std::stop_token stop = {});
    /// Called after a successful native move; preserves object/ontology identity.
    void relocated(const std::string &from, const std::string &to, bool observeContent = true);
    /// Only absent native paths become tombstones; retained bytes remain readable.
    void removed(const std::string &path);
    std::optional<iiFileProvider::ObjectRecord> lookup(const std::string &path) const;
    iiFileProvider::RawEnvelope original(const std::string &documentKey) const;
    /// FTS/keyword/rating/color/collection search, confined to one section. Native
    /// files and the working object are rechecked; stale catalog rows are omitted.
    iiFileProvider::FilePage search(const std::string &section,
        const iiFileProvider::FileFilter &filter = {},
        const std::optional<iiFileProvider::FileCursor> &cursor = {}, std::size_t limit = 256) const;
    iiFileProvider::ObjectStore &objects();
    iiFileProvider::OntologyFileSystem &ontology();
    const std::string &sessionKey(); // Lazy: read/no-op scans create no sessions.
    void endSession();
    /// Identity-scoped package location. Retired/mirror identities retain their
    /// own history without moving an open SQLite database or its WAL files.
    static std::filesystem::path packagePath(const std::filesystem::path &root, const std::string &container);
    static constexpr const char *fileClass = "society:File";
    static constexpr const char *rawClass = "society:Raw";
private:
    class Impl;
    std::unique_ptr<Impl> d;
};
}
