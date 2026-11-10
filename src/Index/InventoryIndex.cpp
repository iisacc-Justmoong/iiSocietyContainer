#include "InventoryIndex.h"
#include <iiFileProviderNative.h>
#include <iiFileProviderPresentation.h>
#include <iiFileProviderStorage.h>
#include <sqlite3.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace iiSocietyContainer {
namespace {
namespace fs = std::filesystem;
using iiFileProvider::native::Presentation;

std::string text(const fs::path &path) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}
fs::path pathFromText(const std::string &value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}
[[noreturn]] void fail(const std::string &message) { throw std::runtime_error(message); }
bool validRelative(const fs::path &path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.lexically_normal() != path) return false;
    for (const auto &part : path) {
        const auto value = text(part);
        if (value.empty() || value == "." || value == ".." || value.find('\0') != value.npos ||
            value.find(':') != value.npos || value.find('\\') != value.npos) return false;
    }
    return true;
}
bool direct(const fs::path &path, bool directory, bool mayBeMissing = false) {
    if (!path.is_absolute() || path.lexically_normal() != path) return false;
    auto current = path.root_path();
    std::error_code error;
    for (const auto &part : path.relative_path()) {
        current /= part;
        const auto status = fs::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory && current == path && mayBeMissing) return true;
        if (error || fs::is_symlink(status) || !fs::exists(status)) return false;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
        if (current != path && !fs::is_directory(status)) return false;
        if (current == path && (directory ? !fs::is_directory(status) : !fs::is_regular_file(status))) return false;
    }
    return true;
}
void validateLocation(const fs::path &root, bool writing) {
    if (!direct(root, true)) fail("Container root is unavailable or redirected");
    const auto directory = root / ".society-runtime/monitor/v1";
    if (!direct(directory, true)) fail("Inventory runtime is unavailable or redirected");
    const auto db = InventoryIndex::path(root);
    if (!direct(db, false, writing)) fail("Inventory database is missing or redirected");
    for (const auto *suffix : {"-wal", "-shm", "-journal"}) {
        auto sidecar = db; sidecar += suffix;
        if (!direct(sidecar, false, true)) fail("Inventory database sidecar is redirected");
    }
}
struct Database {
    sqlite3 *value = nullptr;
    explicit Database(const fs::path &path, bool writing) {
        int flags = (writing ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY) | SQLITE_OPEN_FULLMUTEX;
#ifdef SQLITE_OPEN_NOFOLLOW
        flags |= SQLITE_OPEN_NOFOLLOW;
#endif
        const auto name = text(path);
        const int rc = iiFileProvider::native::openDatabase(name.c_str(), &value, flags);
        if (rc != SQLITE_OK) {
            const auto error = value ? std::string(sqlite3_errmsg(value)) : std::string("SQLite open failed");
            if (value) sqlite3_close(value);
            value = nullptr; fail(error);
        }
        sqlite3_busy_timeout(value, 250);
    }
    ~Database() { if (value) sqlite3_close(value); }
    void exec(const char *sql) {
        char *message = nullptr;
        if (sqlite3_exec(value, sql, nullptr, nullptr, &message) != SQLITE_OK) {
            const auto error = message ? std::string(message) : std::string(sqlite3_errmsg(value));
            sqlite3_free(message); fail(error);
        }
    }
};
struct Statement {
    sqlite3_stmt *value = nullptr;
    sqlite3 *database;
    Statement(Database &db, const char *sql) : database(db.value) {
        if (sqlite3_prepare_v2(database, sql, -1, &value, nullptr) != SQLITE_OK) fail(sqlite3_errmsg(database));
    }
    ~Statement() { sqlite3_finalize(value); }
    void bind(int index, const std::string &text) {
        if (text.size() > std::size_t(std::numeric_limits<int>::max()) ||
            sqlite3_bind_text(value, index, text.data(), int(text.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            fail(sqlite3_errmsg(database));
    }
    void bind(int index, std::int64_t number) {
        if (sqlite3_bind_int64(value, index, number) != SQLITE_OK) fail(sqlite3_errmsg(database));
    }
    bool row() {
        const int code = sqlite3_step(value);
        if (code == SQLITE_ROW) return true;
        if (code == SQLITE_DONE) return false;
        fail(sqlite3_errmsg(database));
    }
    void reset() {
        if (sqlite3_reset(value) != SQLITE_OK || sqlite3_clear_bindings(value) != SQLITE_OK) fail(sqlite3_errmsg(database));
    }
    std::string string(int column) const {
        const auto *data = sqlite3_column_text(value, column);
        const auto size = sqlite3_column_bytes(value, column);
        return data ? std::string(reinterpret_cast<const char *>(data), std::size_t(size)) : std::string{};
    }
    std::int64_t number(int column) const { return sqlite3_column_int64(value, column); }
};
void identity(Database &db, const std::string &id) {
    Statement meta(db, "SELECT schema_version, container_id FROM metadata LIMIT 1");
    if (!meta.row() || meta.number(0) != 1 || meta.string(1) != id || meta.row())
        fail("Inventory identity or schema mismatch");
}
std::int64_t modifiedMs(const fs::file_time_type &value) {
#ifdef _MSC_VER
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(value);
#else
    const auto system = fs::file_time_type::clock::to_sys(value);
#endif
    return std::chrono::duration_cast<std::chrono::milliseconds>(system.time_since_epoch()).count();
}
bool visible(const fs::path &path) {
    for (const auto &part : path) {
        const auto name = text(part);
        if (name.empty() || Presentation::isControlFile(part)) return false;
    }
    return true;
}
bool image(const fs::path &path) {
    auto suffix = text(path.extension());
    std::ranges::transform(suffix, suffix.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return suffix == ".png" || suffix == ".jpg" || suffix == ".jpeg" || suffix == ".webp" ||
           suffix == ".gif" || suffix == ".bmp" || suffix == ".svg";
}
void collect(const fs::path &root, const std::string &id, const InventorySection &section,
             const FileTree::Entry &node, Statement &insert, std::size_t &visited,
             const std::function<bool()> &stillValid) {
    if (++visited % 512 == 0 && stillValid && !stillValid()) fail("Inventory publication was cancelled");
    if (node.kind == FileTree::Kind::Directory && node.relativePath.extension() != ".iiproject" &&
        !node.childrenLoaded)
        fail("Incomplete section cannot be published");
    if (node.relativePath != ".") {
        if (!validRelative(node.relativePath)) fail("Inventory entry has an unsupported path");
        if (!visible(node.relativePath)) return;
        const auto full = pathFromText(section.directory) / node.relativePath;
        if (!validRelative(full) || node.path != root / full) fail("Inventory entry escapes its section");
        const auto physical = text(full), name = text(full.filename()), parent = text(full.parent_path());
        const auto ticks = std::int64_t(node.lastModified.time_since_epoch().count());
        std::string token;
        if (node.kind == FileTree::Kind::File && image(full)) {
            try { token = Presentation::cacheChangeToken(node.path); } catch (...) { token.clear(); }
        }
        auto material = id; material.push_back('\0'); material += physical;
        const auto key = iiFileProvider::native::sha256(material);
        material = key + ':' + std::to_string(node.size) + ':' + std::to_string(ticks) + ':' +
                   std::to_string(unsigned(node.permissions)) + ':' + token;
        const auto revision = iiFileProvider::native::sha256(material);
        if (node.size > std::uint64_t(std::numeric_limits<std::int64_t>::max())) fail("Inventory item is too large");
        insert.bind(1, section.key); insert.bind(2, physical); insert.bind(3, parent); insert.bind(4, name);
        insert.bind(5, key); insert.bind(6, revision); insert.bind(7, token);
        insert.bind(8, std::int64_t(node.size)); insert.bind(9, modifiedMs(node.lastModified));
        insert.bind(10, ticks); insert.bind(11, node.kind == FileTree::Kind::Directory ? std::int64_t(1) : std::int64_t(0));
        if (insert.row()) fail("Unexpected inventory insert result");
        insert.reset();
        // A project package is one provider item, not a second set of assets.
        if (node.kind == FileTree::Kind::Directory && full.extension() == ".iiproject") return;
    }
    for (const auto &child : node.children) collect(root, id, section, child, insert, visited, stillValid);
}
InventoryEntry entry(Statement &statement) {
    InventoryEntry result;
    result.section = statement.string(0); result.path = statement.string(1); result.name = statement.string(2);
    result.key = statement.string(3); result.revision = statement.string(4); result.previewToken = statement.string(5);
    const auto size = statement.number(6);
    if (size < 0) fail("Invalid inventory size");
    result.size = std::uint64_t(size); result.modifiedMs = statement.number(7); result.modifiedTicks = statement.number(8);
    result.directory = statement.number(9) != 0;
    if (!validRelative(pathFromText(result.path)) || result.key.size() != 64 || result.revision.size() != 64)
        fail("Invalid inventory entry");
    return result;
}
}

std::filesystem::path InventoryIndex::path(const fs::path &root) {
    return root / ".society-runtime/monitor/v1/inventory.sqlite";
}

InventoryIndex::Result<bool> InventoryIndex::publish(const fs::path &root, const std::string &id,
    const std::vector<InventorySection> &sections, const std::function<bool()> &stillValid) {
    try {
        if (id.empty() || sections.empty()) fail("Inventory identity and sections are required");
        if (stillValid && !stillValid()) fail("Inventory publication was cancelled");
        validateLocation(root, true);
        Database db(path(root), true);
        bool empty = false;
        {
            Statement tables(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'");
            empty = tables.row() && tables.number(0) == 0;
        }
        if (empty) {
            db.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
            db.exec("BEGIN IMMEDIATE; CREATE TABLE metadata(schema_version INTEGER NOT NULL,container_id TEXT NOT NULL);"
                    "CREATE TABLE sections(key TEXT PRIMARY KEY,directory TEXT UNIQUE NOT NULL,fingerprint TEXT NOT NULL,complete INTEGER NOT NULL);"
                    "CREATE TABLE entries(section TEXT NOT NULL,path TEXT PRIMARY KEY,parent TEXT NOT NULL,name TEXT NOT NULL,"
                    "item_key TEXT NOT NULL,revision TEXT NOT NULL,preview_token TEXT NOT NULL,size INTEGER NOT NULL,"
                    "modified_ms INTEGER NOT NULL,modified_ticks INTEGER NOT NULL,directory INTEGER NOT NULL);"
                    "CREATE INDEX entries_parent ON entries(parent); CREATE INDEX entries_section ON entries(section);"
                    "CREATE UNIQUE INDEX entries_key ON entries(item_key);");
            Statement insert(db, "INSERT INTO metadata VALUES(1,?)"); insert.bind(1, id); (void)insert.row();
            db.exec("COMMIT");
        }
        identity(db, id);
        db.exec("BEGIN IMMEDIATE");
        bool changed = false;
        try {
            std::set<std::string> names, directories;
            Statement previous(db, "SELECT directory,fingerprint,complete FROM sections WHERE key=?");
            Statement remove(db, "DELETE FROM entries WHERE section=?");
            Statement sectionWrite(db, "INSERT INTO sections VALUES(?,?,?,?) ON CONFLICT(key) DO UPDATE SET "
                                       "directory=excluded.directory,fingerprint=excluded.fingerprint,complete=excluded.complete");
            Statement insert(db, "INSERT INTO entries VALUES(?,?,?,?,?,?,?,?,?,?,?)");
            for (const auto &section : sections) {
                if (stillValid && !stillValid()) fail("Inventory publication was cancelled");
                const auto dir = pathFromText(section.directory);
                if (section.key.empty() || !validRelative(dir) || std::distance(dir.begin(), dir.end()) != 1 ||
                    !names.insert(section.key).second || !directories.insert(section.directory).second)
                    fail("Invalid inventory section");
                previous.bind(1, section.key);
                const bool found = previous.row();
                const bool same = found && previous.string(0) == section.directory &&
                    previous.string(1) == std::to_string(section.fingerprint) &&
                    (previous.number(2) != 0) == section.complete;
                previous.reset();
                if (same) continue;
                changed = true;
                if (section.complete) {
                    if (section.tree.relativePath != "." || section.tree.path != root / dir || !section.tree.childrenLoaded)
                        fail("Incomplete section cannot be published");
                    remove.bind(1, section.key); (void)remove.row(); remove.reset();
                    std::size_t visited = 0;
                    collect(root, id, section, section.tree, insert, visited, stillValid);
                }
                sectionWrite.bind(1, section.key); sectionWrite.bind(2, section.directory);
                sectionWrite.bind(3, std::to_string(section.fingerprint)); sectionWrite.bind(4, std::int64_t(section.complete));
                (void)sectionWrite.row(); sectionWrite.reset();
            }
            if (stillValid && !stillValid()) fail("Inventory publication was cancelled");
            validateLocation(root, false);
            db.exec("COMMIT");
        } catch (...) { try { db.exec("ROLLBACK"); } catch (...) {} throw; }
        return changed;
    } catch (const std::exception &error) { return std::unexpected(error.what()); }
}

InventoryIndex::Result<std::vector<InventoryEntry>> InventoryIndex::children(const fs::path &root,
    const std::string &id, const fs::path &folder) {
    try {
        if (id.empty() || !validRelative(folder) || !visible(folder)) fail("Invalid inventory folder");
        validateLocation(root, false);
        Database db(path(root), false); identity(db, id);
        const auto first = text(*folder.begin());
        Statement section(db, "SELECT key,complete FROM sections WHERE directory=?"); section.bind(1, first);
        if (!section.row() || !section.number(1)) fail("Inventory section is unavailable or incomplete");
        const auto key = section.string(0);
        Statement query(db, "SELECT section,path,name,item_key,revision,preview_token,size,modified_ms,modified_ticks,directory "
                            "FROM entries WHERE section=? AND parent=? ORDER BY path");
        query.bind(1, key); query.bind(2, text(folder));
        std::vector<InventoryEntry> result;
        while (query.row()) {
            if (result.size() >= 1000000) fail("Inventory folder exceeds the read limit");
            auto row = entry(query);
            if (row.section != key || pathFromText(row.path).parent_path() != folder || row.name != text(pathFromText(row.path).filename()))
                fail("Inventory row is outside the requested folder");
            result.push_back(std::move(row));
        }
        return result;
    } catch (const std::exception &error) { return std::unexpected(error.what()); }
}
InventoryIndex::Result<std::optional<InventoryEntry>> InventoryIndex::find(const fs::path &root,
    const std::string &id, const std::string &key) {
    try {
        if (id.empty() || key.size() != 64 || !std::ranges::all_of(key, [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            })) fail("Invalid inventory key");
        validateLocation(root, false);
        Database db(path(root), false); identity(db, id);
        Statement query(db, "SELECT e.section,e.path,e.name,e.item_key,e.revision,e.preview_token,e.size,e.modified_ms,"
                            "e.modified_ticks,e.directory FROM entries e JOIN sections s ON s.key=e.section "
                            "WHERE e.item_key=? AND s.complete=1");
        query.bind(1, key);
        if (!query.row()) return std::optional<InventoryEntry>{};
        auto value = entry(query);
        if (value.key != key || query.row()) fail("Invalid inventory lookup result");
        return std::optional<InventoryEntry>{std::move(value)};
    } catch (const std::exception &error) { return std::unexpected(error.what()); }
}
}
