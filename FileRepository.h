#pragma once
#include "iiSocietyContainerExport.h"
#include <OntologyFileSystem.h>
#include <functional>
#include <map>
#include <stop_token>

namespace iiSocietyContainer {
struct FileRepositorySection { std::string key; std::filesystem::path directory; };
struct FileRepositoryContext {
    iiFileProvider::ObjectAuthor author;
    std::string device, description = "Society file operation";
    iiFileProvider::ObjectSessionContext work;
};
struct FileReconciliation { std::size_t files = 0, changed = 0, deleted = 0; };

// A declared association, not proof that a remote copy or publication exists.
// Revision identifies the exact observed content revision the caller pinned.
struct ResourceRelation { std::string id, revision, locator; };
struct ResourceRecord {
    std::string id, containerId, contentRevision, location, entitlementJson;
    std::filesystem::path localLocator;
    std::filesystem::path entitlementFile;
    std::string entitlementFileStatus; // ready, missing, conflict or unavailable
    std::map<std::string, std::string> hashes;
    std::map<std::string, std::string> attributes;
    std::vector<std::string> jobIds, contentRevisionHistory;
    std::vector<ResourceRelation> replicas, publications;
};

/// Native file ownership index. Public files stay ordinary files; immutable RAW
/// originals, revisions, work sessions and semantic facts live inside the drive.
/// Blocking worker-thread API. One owner per thread; independent owners use WAL.
class IISOCIETY_FILETREE_EXPORT FileRepository final {
public:
    using Access = iiFileProvider::ObjectStore::Access;
    enum class Intent { Interactive, UserJob, Background };
    FileRepository(std::filesystem::path root, std::string container,
        std::vector<FileRepositorySection> sections, FileRepositoryContext context = {},
        Access access = Access::ReadWrite, std::function<bool()> available = {});
    FileRepository(std::filesystem::path root, std::string container,
        std::vector<FileRepositorySection> sections, FileRepositoryContext context,
        Access access, std::function<bool()> available, Intent intent);
    ~FileRepository();
    FileRepository(const FileRepository &) = delete;
    FileRepository &operator=(const FileRepository &) = delete;

    /// Serializes a native filesystem mutation with cooperative repository
    /// writers. Calls on this facade may be nested inside the callback. Stop
    /// cancels lock acquisition; once entered, the callback owns completion.
    /// This is not a rollback transaction: callers must retain or repair any
    /// native changes if their callback fails or the process is interrupted.
    void withExclusiveOperation(const std::function<void()> &work, std::stop_token stop = {});

    /// Run rollback-safe background work under the repository lease. A waiting
    /// foreground operation requests cancellation through the supplied token;
    /// false means the work yielded and may be retried. The callback must honor
    /// cancellation and must not publish partial filesystem mutations.
    bool tryBackgroundOperation(const std::function<void(std::stop_token)> &work,
                                std::stop_token stop = {});
    /// A user-requested long operation preempts maintenance, but yields to a
    /// foreground edit. Cancellation and partial-publication rules are the same
    /// as for tryBackgroundOperation.
    bool tryUserJobOperation(const std::function<void(std::stop_token)> &work,
                             std::stop_token stop = {});

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
    // A last-observed resource projection. Unregistered/legacy files return
    // nullopt until intake projects their facts. It never authorizes access.
    std::optional<ResourceRecord> resource(const std::string &path) const;
    void setEntitlement(const std::string &path, const std::string &json);
    // Exports the stored JSON through iiFileProvider; destination must be a new
    // .json file. The source document remains in the object metadata store.
    void exportEntitlement(const std::string &path, const std::filesystem::path &destination) const;
    void setReplica(const std::string &path, const ResourceRelation &relation);
    void setPublication(const std::string &path, const ResourceRelation &relation);
    void setResourceAttribute(const std::string &path, const std::string &key, const std::string &value);
    iiFileProvider::OntologyPage findResourceAttribute(const std::string &key, const std::string &value,
        const iiFileProvider::OntologyCursor &cursor = {}, std::size_t limit = 256) const;
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
    bool cooperativeOperation(const std::function<void(std::stop_token)> &work,
                              std::stop_token stop, bool userJob);
    class Impl;
    std::unique_ptr<Impl> d;
};
}
