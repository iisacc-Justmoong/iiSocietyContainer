#include "FileRepository.h"
#include "FileTree.h"
#include <ObjectSource.h>
#include <TemporaryLease.h>
#include <iiFileProviderStorage.h>
#include <iiFileProviderNative.h>
#include <iiFileProviderPresentation.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

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
constexpr std::array<const char *, 8> colors{"none", "red", "orange", "yellow", "green", "blue", "purple", "gray"};
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
    unsigned lockDepth = 0;
    std::unique_ptr<TemporaryLease> lease;
    struct Operation {
        Impl &owner;
        explicit Operation(Impl &owner, std::stop_token stop = {}) : owner(owner) {
            if (owner.access == Access::ReadOnly) fail("read-only repository");
            if (owner.lockDepth) { ++owner.lockDepth; return; }
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
        const auto subject = OntologyResource::file(file.key);
        const auto original = OntologyResource::file(raw.record.key);
        changes.emplace_back(OntologyTypeAssignment{subject, FileRepository::fileClass});
        changes.emplace_back(OntologyTypeAssignment{original, FileRepository::rawClass});
        std::vector<OntologyAssertion> wanted{
            {subject, "society:original", OntologyValue::resource(original)},
            {subject, "society:catalogKey", OntologyValue::text(catalogKey)},
            {subject, "society:section", OntologyValue::text(split(file.path).first->key)},
            {subject, "society:name", OntologyValue::text(text(pathFromText(file.path).filename()))},
            {subject, "society:title", OntologyValue::text(metadata.title)},
            {subject, "society:color", OntologyValue::text(colors.at(static_cast<unsigned>(metadata.color)))},
            {subject, "society:rating", OntologyValue::integer(metadata.rating)},
            {subject, "society:mediaType", OntologyValue::text(raw.metadata.mediaType)}};
        for (const auto &word : metadata.keywords) wanted.push_back({subject, "society:keyword", OntologyValue::text(word)});
        for (const auto &collection : metadata.collections) wanted.push_back({subject, "society:collection", OntologyValue::text(collection)});
        std::vector<OntologyStatement> existing;
        std::int64_t after = 0;
        for (;;) {
            const auto page = graph->statements(subject, after, 256);
            if (page.empty()) break;
            existing.insert(existing.end(), page.begin(), page.end()); after = page.back().sequence;
        }
        const auto managed = [](const std::string &key) {
            static const std::set<std::string> keys{"society:original","society:catalogKey","society:section","society:name","society:title","society:color","society:rating","society:mediaType","society:keyword","society:collection"};
            return keys.contains(key);
        };
        for (const auto &old : existing) if (managed(old.propertyKey)
            && std::ranges::none_of(wanted, [&](const auto &n) { return n.propertyKey == old.propertyKey && n.value == old.value; }))
            changes.emplace_back(OntologyAssertion{subject, old.propertyKey, old.value, false});
        for (const auto &next : wanted) if (std::ranges::none_of(existing, [&](const auto &old) { return next.propertyKey == old.propertyKey && next.value == old.value; }))
            changes.emplace_back(next);
        // No-op membership checks do not need a new session or graph revision.
        if (changes.size() == 2 && graph->ontologyClass(FileRepository::fileClass)
            && !graph->queryFiles({FileRepository::fileClass, true, {{"society:original", OntologyValue::resource(original)}}}, {}, 1).files.empty()) return;
        for (std::size_t offset = 0; offset < changes.size(); offset += OntologyFileSystem::maximumBatchChanges) {
            const auto end = std::min(changes.size(), offset + OntologyFileSystem::maximumBatchChanges);
            apply({changes.begin() + offset, changes.begin() + end});
        }
    }
};
FileRepository::FileRepository(fs::path root, std::string container, std::vector<FileRepositorySection> sections,
    FileRepositoryContext context, Access access, std::function<bool()> available) : d(std::make_unique<Impl>()) {
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
    d->store = std::make_unique<ObjectStore>(d->directory, std::move(container), access == Access::ReadOnly ? access : Access::Create);
    d->graph = std::make_unique<OntologyFileSystem>(*d->store);
    if (access != Access::ReadOnly) {
        const auto operations = d->directory / "operations";
        if (!fs::exists(operations)) iiFileProvider::native::createDirectory(operations);
        if (!fs::is_directory(operations) || fs::canonical(operations) != operations) fail("operation directory is redirected");
    }
}
FileRepository::~FileRepository() { try { endSession(); } catch (...) {} }
fs::path FileRepository::packagePath(const fs::path &root, const std::string &container) {
    return root / ".society-file-provider" / iiFileProvider::native::sha256(container);
}
ObjectRecord FileRepository::observe(const std::string &logical, const std::optional<FileMetadata> &metadata, std::stop_token stop) {
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
    d->check(); const auto root = d->path(logical);
    if (fs::is_regular_file(root)) { observe(logical, {}, stop); return; }
    if (!fs::is_directory(root)) fail("source is unavailable");
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        if (stop.stop_requested()) fail("capture cancelled");
        if (!visible(it->path().lexically_relative(root)) || it->is_symlink()) { if (it->is_directory()) it.disable_recursion_pending(); continue; }
        if (it->is_regular_file()) observe(logical + '/' + text(it->path().lexically_relative(root)), {}, stop);
    }
}
FileReconciliation FileRepository::reconcile(std::stop_token stop) {
    d->check(); FileReconciliation result;
    for (const auto &part : d->sections) {
        for (fs::recursive_directory_iterator it(part.directory), end; it != end; ++it) {
            if (stop.stop_requested()) fail("reconciliation cancelled");
            if (!visible(it->path().lexically_relative(part.directory)) || it->is_symlink()) { if (it->is_directory()) it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file()) continue;
            if (++result.files > 250000) fail("inventory exceeds 250000 files");
            const auto path = part.key + '/' + text(it->path().lexically_relative(part.directory));
            const auto before = d->store->lookupPath(path); const auto after = observe(path, {}, stop);
            if (!before || before->version != after.version) ++result.changed;
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
            if (!fs::exists(status)) { d->check(); d->store->erase(file.key, file.version, d->begin()); ++result.deleted; }
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
RawEnvelope FileRepository::original(const std::string &key) const { d->check(); return d->store->original(key); }
FilePage FileRepository::search(const std::string &section, const FileFilter &filter, const std::optional<FileCursor> &cursor, std::size_t limit) const {
    if (d->access == Access::ReadOnly && !fs::exists(d->section(section).directory / ".iisacc-file-provider/catalog.sqlite3")) { d->check(); return {}; }
    d->check(); auto page = d->catalog(section).search(filter, cursor, limit);
    std::erase_if(page.files, [&](const auto &row) {
        const auto path = section + '/' + row.path;
        const auto file = d->store->lookupPath(path);
        return !file || !fs::is_regular_file(d->path(path));
    });
    d->check(); return page;
}
ObjectStore &FileRepository::objects() { d->check(); return *d->store; }
OntologyFileSystem &FileRepository::ontology() { d->check(); return *d->graph; }
const std::string &FileRepository::sessionKey() { return d->begin(); }
void FileRepository::endSession() { if (d && d->store && !d->session.empty()) { d->store->endSession(d->session); d->session.clear(); } }
}
