#include "FileRepository.h"
#include "FileTree.h"
#include "src/ManagedOntologyProperties.h"
#include <ObjectSource.h>
#include <TemporaryLease.h>
#include <iiFileProviderStorage.h>
#include <iiFileProviderNative.h>
#include <iiFileProviderNativeDevice.h>
#include <iiFileProviderPresentation.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace iiSocietyContainer {
using namespace iiFileProvider;
namespace fs = std::filesystem;
namespace {
[[noreturn]] void fail(const std::string &message) { throw std::runtime_error("Society files: " + message); }
std::string text(const fs::path &p) { const auto u = p.generic_u8string(); return {u.begin(), u.end()}; }
fs::path pathFromText(const std::string &p) { return fs::path(std::u8string(p.begin(), p.end())); }
bool below(const fs::path &p, const fs::path &root) {
    const auto rel = p.lexically_relative(root); return !rel.empty() && *rel.begin() != ".." && !rel.is_absolute();
}
bool selected(const std::string &path, const std::string &prefix) { return path == prefix || path.starts_with(prefix + '/'); }
bool visible(const fs::path &path) {
    for (const auto &part : path) {
        const auto name = text(part);
        if (name.empty() || name[0] == '.' || name == ".." || name.find_first_of("\\:\0", 0, 3) != std::string::npos
            || native::Presentation::isControlFile(part)) return false;
    }
    return !path.empty() && !path.is_absolute();
}
// A project directory is a single provider item; its internal metadata and
// assets have their own identity and history inside the package.
bool inProjectPackage(const fs::path &path) {
    return std::ranges::any_of(path, [](const auto &part) { return part.extension() == ".iiproject"; });
}
constexpr std::array<const char *, 8> colors{"none", "red", "orange", "yellow", "green", "blue", "purple", "gray"};
constexpr auto defaultEntitlement = R"({"schemaVersion":1,"status":"unassigned","grants":[]})";

// Foreground operations hold a shared intent lease while waiting for and
// executing the repository operation. Background work polls the corresponding
// exclusive lease and cooperatively abandons its current atomic unit when a
// foreground writer arrives. The leases work across processes as well as
// independent repository facades in one process.
class BackgroundGate {
public:
    BackgroundGate(std::vector<fs::path> directories, std::stop_token stop) : m_directories(std::move(directories)), m_stop(stop),
        m_monitor([this](std::stop_token shutdown) { monitor(shutdown); }) {}
    ~BackgroundGate() { m_monitor.request_stop(); m_changed.notify_all(); }
    std::stop_token next() {
        std::unique_lock lock(m_mutex);
        while ((!m_sampled || m_pending) && !m_stop.stop_requested() && !m_failed)
            m_changed.wait_for(lock, std::chrono::milliseconds(20));
        if (m_stop.stop_requested()) fail("background operation cancelled");
        if (m_failed) fail("cannot inspect foreground operation priority");
        m_interrupted = false;
        m_active = std::stop_source{};
        return m_active.get_token();
    }
    bool interrupted() const { std::lock_guard lock(m_mutex); return m_interrupted; }
private:
    void monitor(std::stop_token shutdown) noexcept {
        while (!shutdown.stop_requested()) {
            std::optional<std::stop_source> cancel;
            try {
                bool pending = false;
                for (const auto &directory : m_directories) {
                    TemporaryLease probe(directory, TemporaryLease::Mode::TryExclusive);
                    if (!probe.locked()) { pending = true; break; }
                }
                std::lock_guard lock(m_mutex);
                m_sampled = true; m_pending = pending;
                if (m_pending || m_stop.stop_requested()) {
                    m_interrupted = m_pending;
                    cancel = m_active;
                }
            } catch (...) {
                std::lock_guard lock(m_mutex);
                m_failed = true; cancel = m_active;
            }
            // stop callbacks are arbitrary client code and must not run while
            // the gate mutex or the priority probe is held.
            if (cancel) cancel->request_stop();
            m_changed.notify_all();
            std::unique_lock lock(m_mutex);
            m_changed.wait_for(lock, std::chrono::milliseconds(20), [&] { return shutdown.stop_requested(); });
        }
    }
    std::vector<fs::path> m_directories;
    std::stop_token m_stop;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_sampled = false, m_pending = false, m_interrupted = false, m_failed = false;
    std::stop_source m_active;
    std::jthread m_monitor;
};

// Validate with the SQLite JSON parser already used by the object store. Do not
// silently accept duplicate keys: their interpretation varies between readers.
void validateEntitlement(const std::string &document) {
    if (document.empty() || document.size() > 65536 || document.find('\0') != std::string::npos)
        fail("invalid entitlement JSON size");
    sqlite3 *db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        fail("cannot validate entitlement JSON");
    }
    struct Database { sqlite3 *db; ~Database() { sqlite3_close(db); } } cleanup{db};
    const auto run = [&](const char *sql) {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) fail("entitlement JSON parser unavailable");
        struct Finalize { sqlite3_stmt *statement; ~Finalize() { sqlite3_finalize(statement); } } done{statement};
        if (sqlite3_bind_text(statement, 1, document.data(), int(document.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_step(statement) != SQLITE_ROW) fail("invalid entitlement JSON");
        return sqlite3_column_int(statement, 0) == 1;
    };
    if (!run("SELECT json_valid(?1)") ||
        !run("SELECT json_type(?1)='object' AND NOT EXISTS(SELECT 1 FROM json_tree(?1) WHERE key IS NOT NULL GROUP BY parent,key HAVING count(*)>1)"))
        fail("entitlement must be a JSON object without duplicate keys");
}
void validateRelation(const ResourceRelation &relation) {
    const std::array parts{std::pair{&relation.id, std::size_t(512)}, std::pair{&relation.revision, std::size_t(512)},
                           std::pair{&relation.locator, std::size_t(2048)}};
    for (const auto &[part, limit] : parts)
        if (part->empty() || part->size() > limit || part->find_first_of("\r\n\0", 0, 3) != std::string::npos)
            fail("invalid resource relation");
}
std::string encodeRelation(const ResourceRelation &value) {
    validateRelation(value); return value.id + '\n' + value.revision + '\n' + value.locator;
}
ResourceRelation decodeRelation(const std::string &value) {
    const auto first = value.find('\n');
    const auto second = first == std::string::npos ? first : value.find('\n', first + 1);
    if (first == std::string::npos || second == std::string::npos) fail("invalid stored resource relation");
    ResourceRelation out{value.substr(0,first), value.substr(first+1,second-first-1), value.substr(second+1)};
    validateRelation(out); return out;
}
std::string encodeAttribute(const std::string &key, const std::string &value) {
    if (key.empty() || key.size() > 512 || value.size() > 4096 ||
        key.find('\0') != std::string::npos || value.find('\0') != std::string::npos)
        fail("invalid resource attribute");
    return std::to_string(key.size()) + ':' + key + value;
}
std::pair<std::string, std::string> decodeAttribute(const std::string &text) {
    const auto colon = text.find(':');
    if (colon == std::string::npos) fail("invalid stored resource attribute");
    std::size_t count = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + colon, count);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + colon || count > text.size() - colon - 1)
        fail("invalid stored resource attribute");
    auto key = text.substr(colon + 1, count);
    auto value = text.substr(colon + 1 + count);
    if (encodeAttribute(key, value) != text) fail("noncanonical stored resource attribute");
    return {std::move(key), std::move(value)};
}
std::vector<OntologyStatement> statements(const OntologyFileSystem &graph, const OntologyResource &subject) {
    std::vector<OntologyStatement> result; std::int64_t after = 0;
    for (;;) {
        const auto page = graph.statements(subject, after, 256);
        if (page.empty()) return result;
        result.insert(result.end(), page.begin(), page.end()); after = page.back().sequence;
    }
}
std::string textValue(const std::vector<OntologyStatement> &values, std::string_view key) {
    for (const auto &item : values) if (item.propertyKey == key)
        if (const auto *value = std::get_if<std::string>(&item.value.data)) return *value;
    return {};
}
std::string mediaType(const fs::path &p) {
    auto ext = text(p.extension()); std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".iisc") return "application/vnd.iisacc.canvas";
    if (ext == ".txt" || ext == ".md") return "text/plain";
    return "application/octet-stream";
}
}
class FileRepository::Impl {
public:
    fs::path root, directory;
    std::vector<FileRepositorySection> sections;
    FileRepositoryContext context;
    Access access;
    std::function<bool()> available;
    std::unique_ptr<ObjectStore> store;
    std::unique_ptr<OntologyFileSystem> graph;
    mutable std::map<std::string, std::unique_ptr<FileCatalog>> catalogs;
    std::string session;
    // Repository workers own their facade on one thread. Definitions are
    // checked once per facade, rather than preparing 15 schema queries for
    // every file in a large intake. The graph still validates every write.
    bool resourceSchemaChecked = false;
    bool background = false;
    unsigned lockDepth = 0;
    std::unique_ptr<TemporaryLease> lease;
    struct Operation {
        Impl &owner;
        std::unique_ptr<TemporaryLease> foregroundIntent;
        explicit Operation(Impl &owner, std::stop_token stop = {}) : owner(owner) {
            if (owner.access == Access::ReadOnly) fail("read-only repository");
            if (owner.lockDepth) { ++owner.lockDepth; return; }
            if (!owner.background)
                foregroundIntent = std::make_unique<TemporaryLease>(owner.directory / "foreground",
                    TemporaryLease::Mode::Shared);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            do {
                if (stop.stop_requested()) fail("repository operation cancelled");
                auto candidate = std::make_unique<TemporaryLease>(owner.directory / "operations", TemporaryLease::Mode::TryExclusive);
                if (candidate->locked()) { owner.lease = std::move(candidate); owner.lockDepth = 1; return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (std::chrono::steady_clock::now() < until);
            fail("another repository operation is still active");
        }
        ~Operation() { if (!--owner.lockDepth) owner.lease.reset(); }
    };
    void check() const {
        if ((available && !available()) || !fs::is_directory(root) || fs::canonical(root) != root)
            fail("container is unavailable or replaced");
        for (const auto &part : sections)
            if (!fs::is_directory(part.directory) || fs::canonical(part.directory) != part.directory)
                fail("section is unavailable or redirected");
    }
    const FileRepositorySection &section(const std::string &key) const {
        const auto it = std::ranges::find(sections, key, &FileRepositorySection::key);
        if (it == sections.end()) fail("unknown section"); return *it;
    }
    std::pair<const FileRepositorySection *, std::string> split(const std::string &path) const {
        const auto slash = path.find('/');
        if (slash == std::string::npos || !visible(pathFromText(path))) fail("invalid public file path");
        return {&section(path.substr(0, slash)), path.substr(slash + 1)};
    }
    fs::path path(const std::string &logical) const {
        const auto [part, relative] = split(logical);
        const auto p = part->directory / pathFromText(relative);
        auto current = part->directory;
        for (const auto &leaf : pathFromText(relative)) {
            current /= leaf; std::error_code ec; const auto status = fs::symlink_status(current, ec);
            if (ec && ec != std::errc::no_such_file_or_directory) fail(ec.message());
            if (fs::exists(status) && (fs::is_symlink(status) || fs::canonical(current) != current)) fail("redirected public path");
        }
        return p;
    }
    FileCatalog &catalog(const std::string &key) const {
        auto &value = catalogs[key];
        if (!value) value = std::make_unique<FileCatalog>(section(key).directory,
            access == Access::ReadOnly ? FileCatalog::Access::ReadOnly : FileCatalog::Access::Create);
        return *value;
    }
    const std::string &begin() {
        check();
        if (access == Access::ReadOnly) fail("read-only repository");
        if (context.author.origin.empty()) context.author.origin = "local:unknown";
        if (context.author.subject.empty()) context.author.subject = "unknown";
        if (context.device.empty()) context.device = "unknown";
        if (session.empty()) session = store->beginSession(context.author, context.device, context.description, context.work).key;
        return session;
    }
    void apply(const std::vector<OntologyChange> &changes) {
        // External graph editors still use CAS. A conflicting transaction fails
        // this projection; the next intake recomputes its idempotent facts.
        graph->apply(changes, graph->revision(), begin());
    }
    fs::path entitlementPath(const std::string &key) const {
        return directory / "entitlements" / (iiFileProvider::native::sha256(key) + ".json");
    }
    void ensureEntitlement(const std::string &key, const std::string &document) {
        const auto folder = directory / "entitlements";
        std::error_code ec;
        auto state = fs::symlink_status(folder, ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(state))) {
            iiFileProvider::native::createDirectory(folder);
            ec.clear(); state = fs::symlink_status(folder, ec);
        }
        if (ec || !fs::is_directory(state) || fs::is_symlink(state) || fs::canonical(folder) != folder)
            fail("entitlement directory is unavailable or redirected");
        const auto file = entitlementPath(key);
        ec.clear(); state = fs::symlink_status(file, ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(state))) {
            iiFileProvider::native::File::create(file, document);
            return;
        }
        if (ec || !fs::is_regular_file(state) || fs::is_symlink(state) || fs::canonical(file) != file)
            fail("entitlement file is unavailable or redirected");
        if (iiFileProvider::native::File::read(file, 65536) != document)
            fail("entitlement file differs from the indexed document; preserve and reconcile it explicitly");
    }
    void project(const ObjectRecord &file, const RawEnvelope &raw, const FileMetadata &metadata, const std::string &catalogKey) {
        std::vector<OntologyChange> changes;
        if (!graph->ontologyClass(FileRepository::fileClass)) {
            changes = {OntologyClass{FileRepository::fileClass, "Society file", "", {}},
                OntologyClass{FileRepository::rawClass, "RAW original", "", {}},
                OntologyProperty{"society:original", "Original", OntologyValueKind::Resource, FileRepository::fileClass, FileRepository::rawClass, true}};
            for (const auto *key : {"section", "name", "title", "color", "mediaType"})
                changes.emplace_back(OntologyProperty{std::string("society:") + key, key, OntologyValueKind::Text, FileRepository::fileClass, "", true});
            for (const auto *key : {"keyword", "collection"})
                changes.emplace_back(OntologyProperty{std::string("society:") + key, key, OntologyValueKind::Text, FileRepository::fileClass});
            changes.emplace_back(OntologyProperty{"society:rating", "Rating", OntologyValueKind::Integer, FileRepository::fileClass, "", true});
        }
        if (!graph->property("society:catalogKey")) changes.emplace_back(OntologyProperty{"society:catalogKey", "Catalog identity", OntologyValueKind::Text, FileRepository::fileClass, "", true});
        if (!resourceSchemaChecked) {
            for (const auto *key : {"resourceId", "containerId", "contentRevision", "contentSha256", "originalSha256", "recordValidation", "location", "entitlement", "entitlementSha256"}) {
                const auto property = std::string("society:") + key;
                if (!graph->property(property)) changes.emplace_back(OntologyProperty{property, key, OntologyValueKind::Text, FileRepository::fileClass, "", true});
            }
            for (const auto *key : {"contentRevisionHistory", "jobId", "replicaId", "replicaRecord", "publicationId", "publicationRecord", "attribute"}) {
                const auto property = std::string("society:") + key;
                if (!graph->property(property)) changes.emplace_back(OntologyProperty{property, key, OntologyValueKind::Text, FileRepository::fileClass});
            }
        }
        const auto subject = OntologyResource::file(file.key);
        const auto original = OntologyResource::file(raw.record.key);
        changes.emplace_back(OntologyTypeAssignment{subject, FileRepository::fileClass});
        changes.emplace_back(OntologyTypeAssignment{original, FileRepository::rawClass});
        const auto existing = statements(*graph, subject);
        auto contentRevision = textValue(existing, "society:contentRevision");
        if (contentRevision.empty() || textValue(existing, "society:contentSha256") != file.sha256)
            contentRevision = file.key + "@" + std::to_string(file.version);
        const auto entitlement = textValue(existing, "society:entitlement").empty()
            ? std::string(defaultEntitlement) : textValue(existing, "society:entitlement");
        std::vector<OntologyAssertion> wanted{
            {subject, "society:original", OntologyValue::resource(original)},
            {subject, "society:catalogKey", OntologyValue::text(catalogKey)},
            {subject, "society:section", OntologyValue::text(split(file.path).first->key)},
            {subject, "society:name", OntologyValue::text(text(pathFromText(file.path).filename()))},
            {subject, "society:title", OntologyValue::text(metadata.title)},
            {subject, "society:color", OntologyValue::text(colors.at(static_cast<unsigned>(metadata.color)))},
            {subject, "society:rating", OntologyValue::integer(metadata.rating)},
            {subject, "society:mediaType", OntologyValue::text(raw.metadata.mediaType)},
            {subject, "society:resourceId", OntologyValue::text(file.key)},
            {subject, "society:containerId", OntologyValue::text(store->containerKey())},
            {subject, "society:contentRevision", OntologyValue::text(contentRevision)},
            {subject, "society:contentSha256", OntologyValue::text(file.sha256)},
            {subject, "society:originalSha256", OntologyValue::text(raw.record.sha256)},
            {subject, "society:entitlementSha256", OntologyValue::text(iiFileProvider::native::sha256(entitlement))},
            {subject, "society:recordValidation", OntologyValue::text(file.validationKey)},
            {subject, "society:location", OntologyValue::text(file.path)}};
        for (const auto &word : metadata.keywords) wanted.push_back({subject, "society:keyword", OntologyValue::text(word)});
        for (const auto &collection : metadata.collections) wanted.push_back({subject, "society:collection", OntologyValue::text(collection)});
        for (const auto &old : existing) if (detail::managedOntologyProperty(old.propertyKey)
            && std::ranges::none_of(wanted, [&](const auto &n) { return n.propertyKey == old.propertyKey && n.value == old.value; }))
            changes.emplace_back(OntologyAssertion{subject, old.propertyKey, old.value, false});
        for (const auto &next : wanted) if (std::ranges::none_of(existing, [&](const auto &old) { return next.propertyKey == old.propertyKey && next.value == old.value; }))
            changes.emplace_back(next);
        const auto addOnce = [&](const char *key, const std::string &value) {
            if (std::ranges::none_of(existing, [&](const auto &old) {
                    return old.propertyKey == key && old.value == OntologyValue::text(value);
                })) changes.emplace_back(OntologyAssertion{subject, key, OntologyValue::text(value)});
        };
        if (textValue(existing, "society:entitlement").empty()) addOnce("society:entitlement", defaultEntitlement);
        addOnce("society:contentRevisionHistory", contentRevision);
        // The intake batch is backed by the persisted ObjectStore work session.
        // It is a correlation ID, not a general-purpose job execution guarantee.
        if (!file.sessionKey.empty()) addOnce("society:jobId", "society:intake:" + file.sessionKey);
        if (const auto it = context.work.attributes.find("jobId"); it != context.work.attributes.end() && !it->second.empty())
            addOnce("society:jobId", it->second);
        // No-op membership checks do not need a new session or graph revision.
        if (changes.size() == 2 && graph->ontologyClass(FileRepository::fileClass)
            && !graph->queryFiles({FileRepository::fileClass, true, {{"society:original", OntologyValue::resource(original)}}}, {}, 1).files.empty()) {
            ensureEntitlement(file.key, entitlement);
            resourceSchemaChecked = true;
            return;
        }
        for (std::size_t offset = 0; offset < changes.size(); offset += OntologyFileSystem::maximumBatchChanges) {
            const auto end = std::min(changes.size(), offset + OntologyFileSystem::maximumBatchChanges);
            apply({changes.begin() + offset, changes.begin() + end});
        }
        ensureEntitlement(file.key, entitlement);
        resourceSchemaChecked = true;
    }
    void assignRelation(const std::string &logical, const ResourceRelation &relation, bool replica) {
        const auto encoded = encodeRelation(relation);
        Operation operation(*this);
        check(); (void)path(logical);
        const auto file = store->lookupPath(logical);
        if (!file || file->deleted) fail("resource is not registered");
        const auto subject = OntologyResource::file(file->key);
        const auto existing = statements(*graph, subject);
        if (textValue(existing, "society:resourceId").empty()) fail("resource projection is pending");
        const char *idKey = replica ? "society:replicaId" : "society:publicationId";
        const char *recordKey = replica ? "society:replicaRecord" : "society:publicationRecord";
        std::vector<OntologyChange> changes;
        bool hasId = false, hasRecord = false; std::size_t count = 0;
        for (const auto &old : existing) {
            const auto *value = std::get_if<std::string>(&old.value.data);
            if (!value) continue;
            if (old.propertyKey == idKey && *value == relation.id) hasId = true;
            if (old.propertyKey != recordKey) continue;
            ++count;
            const auto previous = decodeRelation(*value);
            if (previous.id != relation.id) continue;
            if (*value == encoded) hasRecord = true;
            else changes.emplace_back(OntologyAssertion{subject, recordKey, old.value, false});
        }
        if (!hasId && count >= 128) fail("resource relation limit exceeded");
        if (!hasId) changes.emplace_back(OntologyAssertion{subject, idKey, OntologyValue::text(relation.id)});
        if (!hasRecord) changes.emplace_back(OntologyAssertion{subject, recordKey, OntologyValue::text(encoded)});
        if (!changes.empty()) apply(changes);
    }
    void assignAttribute(const std::string &logical, const std::string &key, const std::string &value) {
        const auto encoded = encodeAttribute(key, value);
        Operation operation(*this);
        check(); (void)path(logical);
        const auto file = store->lookupPath(logical);
        if (!file || file->deleted) fail("resource is not registered");
        const auto subject = OntologyResource::file(file->key);
        const auto existing = statements(*graph, subject);
        if (textValue(existing, "society:resourceId").empty()) fail("resource projection is pending");
        std::vector<OntologyChange> changes; std::size_t count = 0; bool present = false, same = false;
        for (const auto &old : existing) {
            if (old.propertyKey != "society:attribute") continue;
            const auto *item = std::get_if<std::string>(&old.value.data);
            if (!item) fail("invalid stored resource attribute type");
            ++count;
            if (decodeAttribute(*item).first != key) continue;
            present = true;
            if (*item == encoded) same = true;
            else changes.emplace_back(OntologyAssertion{subject, "society:attribute", old.value, false});
        }
        if (!present && count >= 128) fail("resource attribute limit exceeded");
        if (!same) changes.emplace_back(OntologyAssertion{subject, "society:attribute", OntologyValue::text(encoded)});
        if (!changes.empty()) apply(changes);
    }
};
FileRepository::FileRepository(fs::path root, std::string container, std::vector<FileRepositorySection> sections,
    FileRepositoryContext context, Access access, std::function<bool()> available)
    : FileRepository(std::move(root), std::move(container), std::move(sections), std::move(context),
                     access, std::move(available), Intent::Interactive) {}
FileRepository::FileRepository(fs::path root, std::string container, std::vector<FileRepositorySection> sections,
    FileRepositoryContext context, Access access, std::function<bool()> available, Intent intent) : d(std::make_unique<Impl>()) {
    if (!root.is_absolute() || sections.empty()) fail("absolute root and sections required");
    d->root = std::move(root); d->sections = std::move(sections); d->context = std::move(context); d->access = access; d->available = std::move(available);
    std::set<std::string> keys;
    for (const auto &part : d->sections)
        if (part.key.empty() || part.key.find('/') != std::string::npos || !visible(pathFromText(part.key))
            || !part.directory.is_absolute() || !below(part.directory, d->root) || !keys.insert(part.key).second) fail("invalid repository section");
    if (container.empty() || container.size() > 4096 || container.find('\0') != std::string::npos) fail("invalid container identity");
    d->check(); const auto state = d->root / ".society-file-provider";
    if (access != Access::ReadOnly && !fs::exists(state)) iiFileProvider::native::createDirectory(state);
    if (!fs::is_directory(state) || fs::canonical(state) != state) fail("private storage root is redirected or unavailable");
    d->directory = packagePath(d->root, container);
    if (access != Access::ReadOnly && !fs::exists(d->directory)) iiFileProvider::native::createDirectory(d->directory);
    if (!fs::is_directory(d->directory) || fs::canonical(d->directory) != d->directory) fail("private repository redirected or unavailable");
    std::unique_ptr<TemporaryLease> openingIntent;
    const auto prepareCoordination = [&] {
        const auto operations = d->directory / "operations";
        if (!fs::exists(operations)) iiFileProvider::native::createDirectory(operations);
        if (!fs::is_directory(operations) || fs::canonical(operations) != operations) fail("operation directory is redirected");
        const auto foreground = d->directory / "foreground";
        if (!fs::exists(foreground)) iiFileProvider::native::createDirectory(foreground);
        if (!fs::is_directory(foreground) || fs::canonical(foreground) != foreground) fail("foreground directory is redirected");
        const auto intake = d->directory / "intake";
        if (!fs::exists(intake)) iiFileProvider::native::createDirectory(intake);
        if (!fs::is_directory(intake) || fs::canonical(intake) != intake) fail("intake directory is redirected");
        const auto jobs = d->directory / "jobs";
        if (!fs::exists(jobs)) iiFileProvider::native::createDirectory(jobs);
        if (!fs::is_directory(jobs) || fs::canonical(jobs) != jobs) fail("job priority directory is redirected");
        if (intent != Intent::Background)
            openingIntent = std::make_unique<TemporaryLease>(
                intent == Intent::Interactive ? foreground : jobs, TemporaryLease::Mode::Shared);
    };
    // A new ObjectStore deliberately refuses a nonempty unknown package. Do
    // not create coordination directories until it has initialized the empty
    // package. For an existing store, announce intent before opening SQLite.
    const bool existingStore = fs::exists(d->directory / "objects.sqlite3");
    if (access != Access::ReadOnly && existingStore) prepareCoordination();
    // Announce user intent before opening SQLite: its initialization can wait
    // on a background transaction before the caller reaches an operation.
    d->store = std::make_unique<ObjectStore>(d->directory, std::move(container), access == Access::ReadOnly ? access : Access::Create);
    if (access != Access::ReadOnly && !existingStore) prepareCoordination();
    d->graph = std::make_unique<OntologyFileSystem>(*d->store);
}
FileRepository::~FileRepository() { try { endSession(); } catch (...) {} }
fs::path FileRepository::packagePath(const fs::path &root, const std::string &container) {
    return root / ".society-file-provider" / iiFileProvider::native::sha256(container);
}
void FileRepository::withExclusiveOperation(const std::function<void()> &work, std::stop_token stop) {
    if (!work) fail("repository operation callback is required");
    if (stop.stop_requested()) fail("repository operation cancelled");
    Impl::Operation operation(*d, stop);
    d->check();
    if (stop.stop_requested()) fail("repository operation cancelled");
    work();
}
bool FileRepository::tryBackgroundOperation(const std::function<void(std::stop_token)> &work, std::stop_token stop) {
    return cooperativeOperation(work, stop, false);
}
bool FileRepository::tryUserJobOperation(const std::function<void(std::stop_token)> &work, std::stop_token stop) {
    return cooperativeOperation(work, stop, true);
}
bool FileRepository::cooperativeOperation(const std::function<void(std::stop_token)> &work, std::stop_token stop, bool userJob) {
    if (!work || d->background || d->lockDepth) fail("invalid background operation");
    if (d->access == Access::ReadOnly) fail("read-only repository");
    if (stop.stop_requested()) fail("background operation cancelled");
    std::unique_ptr<TemporaryLease> jobIntent;
    if (userJob) jobIntent = std::make_unique<TemporaryLease>(d->directory / "jobs", TemporaryLease::Mode::Shared);
    std::vector<fs::path> priority{d->directory / "foreground"};
    if (!userJob) priority.push_back(d->directory / "jobs");
    BackgroundGate gate(std::move(priority), stop);
    const auto token = gate.next();
    struct BackgroundScope {
        bool &value;
        explicit BackgroundScope(bool &flag) : value(flag) { value = true; }
        ~BackgroundScope() { value = false; }
    } scope(d->background);
    try {
        Impl::Operation operation(*d, token);
        d->check();
        if (token.stop_requested()) {
            if (stop.stop_requested()) fail("background operation cancelled");
            return false;
        }
        work(token);
    } catch (...) {
        if (!stop.stop_requested() && token.stop_requested() && gate.interrupted()) return false;
        throw;
    }
    if (stop.stop_requested()) fail("background operation cancelled");
    return !token.stop_requested();
}
ObjectRecord FileRepository::observe(const std::string &logical, const std::optional<FileMetadata> &metadata, std::stop_token stop) {
    if (d->access == Access::ReadOnly) fail("read-only repository");
    // An explicit file observation is a user-requested long operation. It
    // takes priority over indexing, but a different interactive file edit may
    // interrupt its cancellable snapshot and run before it retries. Callers
    // that already own an operation keep their existing atomic boundary.
    if (!d->background && !d->lockDepth) {
        ObjectRecord result;
        while (!stop.stop_requested()) {
            if (tryUserJobOperation([&](std::stop_token token) { result = observe(logical, metadata, token); }, stop))
                return result;
        }
        fail("file observation cancelled");
    }
    if (inProjectPackage(pathFromText(logical).parent_path())) fail("project package internals are not separate file records");
    Impl::Operation operation(*d, stop);
    d->check(); if (stop.stop_requested()) fail("reconciliation cancelled");
    const auto native = d->path(logical); const auto [part, relative] = d->split(logical);
    const auto tree = FileTree(part->directory).entry(pathFromText(relative));
    if (!tree || tree.value->kind != FileTree::Kind::File) fail("choose a resident regular file");
    auto &catalog = d->catalog(part->key);
    const auto classification = catalog.registerFile(relative, metadata);
    if (!classification.metadataWritten) fail(classification.metadataError);
    const auto identity = ObjectSource::inspect(native);
    auto current = d->store->lookupPath(logical);
    if (!current && d->graph->property("society:catalogKey")) {
        const auto previous = d->graph->queryFiles({fileClass, true, {{"society:catalogKey", OntologyValue::text(classification.file.key)}, {"society:section", OntologyValue::text(part->key)}}}, {}, 2);
        if (previous.files.size() > 1) fail("ambiguous catalog identity");
        if (!previous.files.empty()) {
            const auto old = previous.files.front();
            if (fs::exists(d->path(old.path))) fail("working file identity is already resident");
            current = d->store->move(old.key, logical, old.version, d->begin());
        }
    }
    if (!current) current = d->store->importDocument(native, logical,
        {text(native.filename()), mediaType(native), classification.file.metadata}, d->begin(), {{}, {}, stop}).file;
    else if (current->sourceStamp != identity.stamp) {
        ObjectSource snapshot(native, d->directory, stop, identity);
        current = d->store->reviseFile(current->key, snapshot.path(), current->version, d->begin(), {{}, identity.stamp, stop});
    }
    auto original = d->store->original(current->key);
    if (original.metadata.annotations != classification.file.metadata) {
        original.record = d->store->reviseMetadata(original.record.key, classification.file.metadata, original.record.version, d->begin());
        original.metadata.annotations = classification.file.metadata;
    }
    d->project(*current, original, classification.file.metadata, classification.file.key);
    d->check(); return *current;
}
void FileRepository::captureTree(const std::string &logical, std::stop_token stop) {
    if (inProjectPackage(pathFromText(logical).parent_path())) fail("project package internals are managed as one package");
    d->check(); const auto root = d->path(logical);
    if (fs::is_regular_file(root)) { observe(logical, {}, stop); return; }
    if (!fs::is_directory(root)) fail("source is unavailable");
    if (root.extension() == ".iiproject") fail("project package internals are managed as one package");
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        if (stop.stop_requested()) fail("capture cancelled");
        if (it->is_directory() && it->path().extension() == ".iiproject") { it.disable_recursion_pending(); continue; }
        if (!visible(it->path().lexically_relative(root)) || it->is_symlink()) { if (it->is_directory()) it.disable_recursion_pending(); continue; }
        if (it->is_regular_file()) observe(logical + '/' + text(it->path().lexically_relative(root)), {}, stop);
    }
}
FileReconciliation FileRepository::reconcile(std::stop_token stop) {
    if (d->background || d->lockDepth) fail("reconciliation cannot be nested in an operation");
    d->check(); FileReconciliation result;
    // The mirror and synchronizer can request intake independently. Only one
    // pass per container runs at a time; waiting never owns the writer lease.
    std::unique_ptr<TemporaryLease> intake;
    while (!stop.stop_requested()) {
        auto candidate = std::make_unique<TemporaryLease>(d->directory / "intake", TemporaryLease::Mode::TryExclusive);
        if (candidate->locked()) { intake = std::move(candidate); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!intake) fail("reconciliation cancelled");
    BackgroundGate gate({d->directory / "foreground", d->directory / "jobs"}, stop);
    struct BackgroundScope {
        bool &value;
        explicit BackgroundScope(bool &flag) : value(flag) { value = true; }
        ~BackgroundScope() { value = false; }
    } scope(d->background);
    const auto perform = [&](const auto &work) {
        for (;;) {
            const auto token = gate.next();
            try {
                work(token);
                if (!token.stop_requested()) return;
            } catch (...) {
                if (stop.stop_requested()) fail("reconciliation cancelled");
                if (!token.stop_requested() || !gate.interrupted()) throw;
            }
            if (stop.stop_requested()) fail("reconciliation cancelled");
            if (!gate.interrupted()) fail("background operation interrupted");
        }
    };
    for (const auto &part : d->sections) {
        for (fs::recursive_directory_iterator it(part.directory), end; it != end; ++it) {
            if (stop.stop_requested()) fail("reconciliation cancelled");
            if (it->is_directory() && it->path().extension() == ".iiproject") { it.disable_recursion_pending(); continue; }
            if (!visible(it->path().lexically_relative(part.directory)) || it->is_symlink()) { if (it->is_directory()) it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file()) continue;
            if (++result.files > 250000) fail("inventory exceeds 250000 files");
            const auto relative = text(it->path().lexically_relative(part.directory));
            const auto path = part.key + '/' + relative;
            const auto before = d->store->lookupPath(path);
            std::optional<ObjectRecord> after;
            auto extension = text(it->path().extension());
            std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            const bool deletedModel = part.key == "deleted" &&
                (extension == ".safetensors" || extension == ".safetensor" || extension == ".gguf" || extension == ".iildmodel");
            if ((part.key == "models" || deletedModel) && !before) {
                // Models are ordinary resident files. Their classification and
                // stable catalog identity do not require a second copy in the
                // immutable object store. An already tracked legacy model can
                // still be reconnected by its prior catalog identity.
                perform([&](std::stop_token token) {
                    if (!fs::is_regular_file(d->path(path))) return;
                    if (token.stop_requested()) fail("reconciliation cancelled");
                    auto &catalog = d->catalog(part.key);
                    const auto previous = catalog.lookupPath(relative);
                    if (previous) {
                        // A different file at an old pathname must not inherit
                        // the old catalog identity merely because names match.
                        try { (void)catalog.read(previous->key, 0, 1); }
                        catch (const std::exception &) { fail("model catalog identity conflict; resolve the existing entry before indexing"); }
                    }
                    const auto registration = catalog.registerFile(relative);
                    if (!registration.metadataWritten) fail(registration.metadataError);
                    if (d->graph->property("society:catalogKey")) {
                        std::vector<OntologyCondition> conditions{{"society:catalogKey", OntologyValue::text(registration.file.key)}};
                        if (!deletedModel) conditions.push_back({"society:section", OntologyValue::text(part.key)});
                        const auto legacy = d->graph->queryFiles({fileClass, true, conditions}, {}, 2);
                        if (legacy.files.size() > 1) fail("ambiguous catalog identity");
                        if (!legacy.files.empty()) after = observe(path, {}, token);
                    }
                    if (!previous || previous->revision != registration.file.revision) ++result.changed;
                });
                continue;
            }
            perform([&](std::stop_token token) {
                // A foreground rename can remove this candidate while intake
                // yields. Its destination is discovered by a later pass.
                if (fs::is_regular_file(d->path(path))) after = observe(path, {}, token);
            });
            if (after && (!before || before->version != after->version)) ++result.changed;
        }
    }
    // A failed/cancelled/partial scan never reaches deletion reconciliation.
    d->check(); std::int64_t cursor = 0;
    for (;;) {
        const auto page = d->store->index(cursor, 1024);
        if (page.empty()) break;
        for (const auto &file : page) {
            cursor = file.indexKey; if (file.path.starts_with(".originals/")) continue;
            if (stop.stop_requested()) fail("reconciliation cancelled");
            const auto native = d->path(file.path); std::error_code ec;
            const auto status = fs::symlink_status(native, ec);
            if (ec && ec != std::errc::no_such_file_or_directory) fail(ec.message());
            if (!fs::exists(status)) {
                // The file may be in the middle of a native move. Serialize
                // against its index update and recheck both the record and
                // filesystem after acquiring the same cooperative lease.
                perform([&](std::stop_token token) {
                    Impl::Operation operation(*d, token);
                    d->check();
                    if (token.stop_requested()) fail("reconciliation cancelled");
                    const auto current = d->store->lookup(file.key);
                    if (!current || current->deleted || current->path != file.path || current->version != file.version) return;
                    ec.clear();
                    const auto currentStatus = fs::symlink_status(d->path(current->path), ec);
                    if (ec && ec != std::errc::no_such_file_or_directory) fail(ec.message());
                    if (!fs::exists(currentStatus)) { d->store->erase(current->key, current->version, d->begin()); ++result.deleted; }
                });
            }
        }
    }
    return result;
}
void FileRepository::relocated(const std::string &from, const std::string &to, bool observeContent) {
    Impl::Operation operation(*d);
    d->check(); (void)d->path(from); (void)d->path(to); std::int64_t after = 0;
    for (;;) {
        const auto page = d->store->index(after, 1024); if (page.empty()) break;
        for (const auto &file : page) {
            after = file.indexKey; if (!selected(file.path, from)) continue;
            const auto destination = to + file.path.substr(from.size());
            const auto native = d->path(destination);
            if (!fs::is_regular_file(native) || fs::exists(d->path(file.path))) fail("move is not complete");
            const auto original = d->store->original(file.key);
            const auto previous = original.metadata.annotations;
            const auto [oldSection, oldRelative] = d->split(file.path);
            const auto [newSection, newRelative] = d->split(destination);
            if (observeContent && oldSection->key != newSection->key) {
                const auto sourceCompanion = pathFromText(text(d->path(file.path)) + ".iisacc.json");
                const auto targetCompanion = pathFromText(text(native) + ".iisacc.json");
                if (!fs::exists(targetCompanion) && fs::exists(sourceCompanion))
                    iiFileProvider::native::copyFile(sourceCompanion, targetCompanion);
            }
            if (!observeContent) {
                const auto moved = d->store->move(file.key, destination, file.version, d->begin());
                const auto oldCatalog = d->catalog(oldSection->key).lookupPath(oldRelative);
                d->project(moved, original, previous, oldCatalog ? oldCatalog->key : file.key);
                continue;
            }
            const auto registered = d->catalog(newSection->key).registerFile(newRelative,
                oldSection->key == newSection->key ? std::optional<FileMetadata>(previous) : std::nullopt);
            if (!registered.metadataWritten) fail(registered.metadataError);
            const auto stamp = ObjectSource::inspect(native);
            d->store->move(file.key, destination, file.version, d->begin(), {{}, stamp.stamp});
            observe(destination, oldSection->key == newSection->key ? std::optional<FileMetadata>(previous) : std::nullopt);
        }
    }
}
void FileRepository::removed(const std::string &path) {
    Impl::Operation operation(*d);
    d->check(); (void)d->path(path); std::int64_t after = 0;
    for (;;) {
        const auto page = d->store->index(after, 1024); if (page.empty()) break;
        for (const auto &file : page) {
            after = file.indexKey; if (!selected(file.path, path)) continue;
            if (fs::exists(d->path(file.path))) fail("refusing to tombstone an existing file");
            d->store->erase(file.key, file.version, d->begin());
        }
    }
}
std::optional<ObjectRecord> FileRepository::lookup(const std::string &path) const { d->check(); (void)d->split(path); return d->store->lookupPath(path); }
std::optional<ResourceRecord> FileRepository::resource(const std::string &path) const {
    const auto file = lookup(path);
    if (!file || file->deleted) return {};
    const auto existing = statements(*d->graph, OntologyResource::file(file->key));
    ResourceRecord result;
    result.id = textValue(existing, "society:resourceId");
    if (result.id.empty()) return {};
    result.containerId = textValue(existing, "society:containerId");
    result.contentRevision = textValue(existing, "society:contentRevision");
    result.location = textValue(existing, "society:location");
    result.localLocator = d->path(path);
    result.entitlementJson = textValue(existing, "society:entitlement");
    result.entitlementFile = d->entitlementPath(file->key);
    result.entitlementFileStatus = "unavailable";
    try {
        const auto folder = result.entitlementFile.parent_path();
        std::error_code ec;
        const auto folderState = fs::symlink_status(folder, ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(folderState))) result.entitlementFileStatus = "missing";
        else if (!ec && fs::is_directory(folderState) && !fs::is_symlink(folderState) && fs::canonical(folder) == folder) {
            ec.clear(); const auto fileState = fs::symlink_status(result.entitlementFile, ec);
            if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(fileState))) result.entitlementFileStatus = "missing";
            else if (!ec && fs::is_regular_file(fileState) && !fs::is_symlink(fileState) && fs::canonical(result.entitlementFile) == result.entitlementFile)
                result.entitlementFileStatus = iiFileProvider::native::File::read(result.entitlementFile, 65536) == result.entitlementJson ? "ready" : "conflict";
        }
    } catch (...) { result.entitlementFileStatus = "unavailable"; }
    result.hashes.emplace("sha256", textValue(existing, "society:contentSha256"));
    result.hashes.emplace("original.sha256", textValue(existing, "society:originalSha256"));
    result.hashes.emplace("entitlement.sha256", textValue(existing, "society:entitlementSha256"));
    result.hashes.emplace("record.validation", textValue(existing, "society:recordValidation"));
    for (const auto &item : existing) {
        const auto *value = std::get_if<std::string>(&item.value.data);
        if (!value) continue;
        if (item.propertyKey == "society:jobId") result.jobIds.push_back(*value);
        else if (item.propertyKey == "society:contentRevisionHistory") result.contentRevisionHistory.push_back(*value);
        else if (item.propertyKey == "society:replicaRecord") result.replicas.push_back(decodeRelation(*value));
        else if (item.propertyKey == "society:publicationRecord") result.publications.push_back(decodeRelation(*value));
        else if (item.propertyKey == "society:attribute") {
            auto [key, data] = decodeAttribute(*value);
            if (!result.attributes.emplace(std::move(key), std::move(data)).second) fail("duplicate stored resource attribute");
        }
    }
    return result;
}
void FileRepository::setEntitlement(const std::string &path, const std::string &json) {
    validateEntitlement(json);
    Impl::Operation operation(*d);
    d->check(); (void)d->path(path);
    const auto file = d->store->lookupPath(path);
    if (!file || file->deleted) fail("resource is not registered");
    const auto subject = OntologyResource::file(file->key);
    const auto existing = statements(*d->graph, subject);
    if (textValue(existing, "society:resourceId").empty()) fail("resource projection is pending");
    const auto previous = textValue(existing, "society:entitlement");
    if (previous.empty()) fail("resource entitlement is unavailable");
    d->ensureEntitlement(file->key, previous);
    std::vector<OntologyChange> changes;
    const auto digest = iiFileProvider::native::sha256(json);
    bool hasDocument = false, hasDigest = false;
    for (const auto &old : existing) {
        if (old.propertyKey == "society:entitlement") {
            if (old.value == OntologyValue::text(json)) hasDocument = true;
            else changes.emplace_back(OntologyAssertion{subject, old.propertyKey, old.value, false});
        } else if (old.propertyKey == "society:entitlementSha256") {
            if (old.value == OntologyValue::text(digest)) hasDigest = true;
            else changes.emplace_back(OntologyAssertion{subject, old.propertyKey, old.value, false});
        }
    }
    if (!hasDocument) changes.emplace_back(OntologyAssertion{subject, "society:entitlement", OntologyValue::text(json)});
    if (!hasDigest) changes.emplace_back(OntologyAssertion{subject, "society:entitlementSha256", OntologyValue::text(digest)});
    if (changes.empty()) return;
    const auto destination = d->entitlementPath(file->key);
    if (previous != json) iiFileProvider::native::File::update(destination, previous, json);
    try { d->apply(changes); }
    catch (...) {
        try { if (previous != json) iiFileProvider::native::File::update(destination, json, previous); }
        catch (...) { fail("entitlement index update failed and the JSON requires reconciliation"); }
        throw;
    }
}
void FileRepository::exportEntitlement(const std::string &path, const fs::path &destination) const {
    const auto record = resource(path);
    if (!record || record->entitlementJson.empty()) fail("resource entitlement is unavailable");
    if (destination.extension() != ".json") fail("entitlement export must use a .json destination");
    iiFileProvider::native::AtomicSave output;
    output.open(destination);
    if (output.device().write(record->entitlementJson.data(), std::int64_t(record->entitlementJson.size())) != std::int64_t(record->entitlementJson.size()))
        fail("cannot write entitlement JSON");
    const auto result = output.commit(false);
    if (!result.succeeded) fail("cannot publish entitlement JSON: " + result.message);
}
void FileRepository::setReplica(const std::string &path, const ResourceRelation &relation) { d->assignRelation(path, relation, true); }
void FileRepository::setPublication(const std::string &path, const ResourceRelation &relation) { d->assignRelation(path, relation, false); }
void FileRepository::setResourceAttribute(const std::string &path, const std::string &key, const std::string &value) {
    d->assignAttribute(path, key, value);
}
OntologyPage FileRepository::findResourceAttribute(const std::string &key, const std::string &value,
    const OntologyCursor &cursor, std::size_t limit) const {
    d->check();
    const auto term = encodeAttribute(key, value);
    if (!limit || limit > 1024) fail("resource search limit must be 1 to 1024");
    if (!d->graph->property("society:attribute")) {
        if (cursor.revision || cursor.fileSequence || cursor.afterIndexKey || !cursor.fingerprint.empty())
            fail("resource search cursor is no longer valid");
        return {};
    }
    auto page = d->graph->queryFiles({fileClass, true, {{"society:attribute", OntologyValue::text(term)}}}, cursor, limit);
    std::erase_if(page.files, [&](const auto &entry) {
        try {
            const auto [section, relative] = d->split(entry.path);
            const auto file = FileTree(section->directory).entry(pathFromText(relative));
            const auto current = d->store->lookupPath(entry.path);
            return !file || file.value->kind != FileTree::Kind::File || !current || current->key != entry.key;
        } catch (...) { return true; }
    });
    return page;
}
RawEnvelope FileRepository::original(const std::string &key) const { d->check(); return d->store->original(key); }
FilePage FileRepository::search(const std::string &section, const FileFilter &filter, const std::optional<FileCursor> &cursor, std::size_t limit) const {
    if (d->access == Access::ReadOnly && !fs::exists(d->section(section).directory / ".iisacc-file-provider/catalog.sqlite3")) { d->check(); return {}; }
    d->check(); auto page = d->catalog(section).search(filter, cursor, limit);
    std::erase_if(page.files, [&](const auto &row) {
        const auto path = section + '/' + row.path;
        const auto file = d->store->lookupPath(path);
        return (!file && section != "models") || !fs::is_regular_file(d->path(path));
    });
    d->check(); return page;
}
ObjectStore &FileRepository::objects() { d->check(); return *d->store; }
OntologyFileSystem &FileRepository::ontology() { d->check(); return *d->graph; }
const std::string &FileRepository::sessionKey() { return d->begin(); }
void FileRepository::endSession() { if (d && d->store && !d->session.empty()) { d->store->endSession(d->session); d->session.clear(); } }
}
