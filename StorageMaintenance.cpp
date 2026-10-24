#include <iiFileProviderStorage.h>
#include <iiFileProviderPresentation.h>
#include <iiFileProviderNative.h>
#include "StorageMaintenance.h"
#include <TemporaryLease.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace iiSocietyContainer {
namespace {
namespace fs = std::filesystem;
using Report = StorageMaintenance::Report;
using Lease = iiFileProvider::TemporaryLease;
constexpr std::array sections{"Asset Library", "Deleted", "Files", "Forked", "Generation History", "Models", "Photos", "Published", "Thinking Space"};

bool direct(const fs::path& path, bool directory) {
    if (!path.is_absolute() || path.lexically_normal() != path) return false;
    auto prefix = path.root_path();
    for (const auto& part : path.relative_path()) {
        prefix /= part;
        std::error_code ec;
        const auto status = fs::symlink_status(prefix, ec);
        if (ec || fs::is_symlink(status) || !fs::exists(status)) return false;
        if (prefix != path && !fs::is_directory(status)) return false;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(prefix.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
        if (prefix == path && (directory ? !fs::is_directory(status) : !fs::is_regular_file(status))) return false;
    }
    return true;
}

struct Identity {
    std::uint64_t volume = 0, node = 0;
    bool operator==(const Identity&) const = default;
};
std::optional<Identity> identity(const fs::path& path) {
    if (!direct(path, true)) return {};
#ifdef _WIN32
    const auto handle = iiFileProvider::native::createFile(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid = GetFileInformationByHandle(handle, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    CloseHandle(handle);
    if (!valid) return {};
    return Identity{info.dwVolumeSerialNumber, (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
#else
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return {};
    return Identity{static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino)};
#endif
}
bool uuid(std::string_view value) {
    if (value.size() != 36) return false;
    bool nonzero = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
        else {
            const auto c = value[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
            nonzero |= c != '0';
        }
    }
    return nonzero;
}

// The bounded manifest reader validates complete JSON syntax while retaining
// only the three scalar identity fields. It does not allocate a document tree.
class Manifest final {
    std::string_view input;
    std::size_t position = 0;
    void whitespace() { while (position < input.size() && (input[position] == ' ' || input[position] == '\n' || input[position] == '\r' || input[position] == '\t')) ++position; }
    bool take(char c) { whitespace(); if (position == input.size() || input[position] != c) return false; ++position; return true; }
    bool string(std::string* result = nullptr) {
        if (!take('"')) return false;
        while (position < input.size()) {
            auto c = input[position++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 32) return false;
            if (c == '\\') {
                if (position == input.size()) return false;
                c = input[position++];
                if (c == 'u') {
                    if (position + 4 > input.size()) return false;
                    for (int i = 0; i < 4; ++i) {
                        const auto h = input[position++];
                        if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) return false;
                    }
                    if (result) result->push_back('?');
                    continue;
                }
                if (std::string_view("\"\\/bfnrt").find(c) == std::string_view::npos) return false;
            }
            if (result) result->push_back(c);
        }
        return false;
    }
    bool number() {
        whitespace();
        if (position < input.size() && input[position] == '-') ++position;
        if (position == input.size()) return false;
        if (input[position] == '0') ++position;
        else {
            if (input[position] < '1' || input[position] > '9') return false;
            while (position < input.size() && input[position] >= '0' && input[position] <= '9') ++position;
        }
        if (position < input.size() && input[position] == '.') {
            ++position; const auto start = position;
            while (position < input.size() && input[position] >= '0' && input[position] <= '9') ++position;
            if (start == position) return false;
        }
        if (position < input.size() && (input[position] == 'e' || input[position] == 'E')) {
            ++position; if (position < input.size() && (input[position] == '+' || input[position] == '-')) ++position;
            const auto start = position;
            while (position < input.size() && input[position] >= '0' && input[position] <= '9') ++position;
            if (start == position) return false;
        }
        return true;
    }
    bool value(unsigned depth) {
        if (depth > 24) return false;
        whitespace(); if (position == input.size()) return false;
        if (input[position] == '"') return string();
        if (input[position] == '{') {
            ++position; if (take('}')) return true;
            do { if (!string() || !take(':') || !value(depth + 1)) return false; if (take('}')) return true; } while (take(','));
            return false;
        }
        if (input[position] == '[') {
            ++position; if (take(']')) return true;
            do { if (!value(depth + 1)) return false; if (take(']')) return true; } while (take(','));
            return false;
        }
        for (auto literal : {"true", "false", "null"}) {
            if (input.substr(position).starts_with(literal)) { position += std::char_traits<char>::length(literal); return true; }
        }
        return number();
    }
public:
    explicit Manifest(std::string_view text) : input(text) {}
    bool matches(std::string_view expected) {
        if (!take('{')) return false;
        bool type = false, schema = false, id = false;
        do {
            std::string key;
            if (!string(&key) || !take(':')) return false;
            if (key == "type" || key == "identifier") {
                std::string scalar; if (!string(&scalar)) return false;
                if (key == "type") { if (type || scalar != "SocietyDrive") return false; type = true; }
                else { if (id || scalar != expected || !uuid(scalar)) return false; id = true; }
            } else if (key == "schemaVersion") {
                whitespace(); const auto start = position;
                if (schema || !number() || input.substr(start, position - start) != "1") return false;
                schema = true;
            } else if (!value(1)) return false;
            if (take('}')) { whitespace(); return position == input.size() && type && schema && id; }
        } while (take(','));
        return false;
    }
};

bool legacy(const fs::path& root) {
    std::error_code ec;
    const auto marker = fs::symlink_status(root / ".society-disk.plist", ec);
    if (fs::exists(marker) || fs::is_symlink(marker)) return true;
    const auto info = fs::symlink_status(root / "Info.plist", ec);
    return (fs::exists(info) || fs::is_symlink(info)) && fs::exists(root / "bands", ec);
}
bool validRoot(const fs::path& root, std::string_view expected, const std::optional<Identity>& original = {}) {
    const auto current = identity(root);
    auto extension = root.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char value) { return std::tolower(value); });
    if (extension != ".societycontainer" || !uuid(expected) || !current
        || (original && current != original) || legacy(root)) return false;
    for (auto parent = root.parent_path(); !parent.empty();) {
        if (legacy(parent)) return false;
        std::error_code ec;
        if (fs::exists(fs::symlink_status(parent / ".society-drive.json", ec))) return false;
        const auto next = parent.parent_path(); if (next == parent) break; parent = next;
    }
    const auto manifest = root / ".society-drive.json";
    if (!direct(manifest, false)) return false;
    std::error_code ec; const auto size = fs::file_size(manifest, ec);
    if (ec || size == 0 || size > 16384) return false;
    // The provider's Windows reader permits atomic replacement while this
    // maintenance snapshot is open. A CRT ifstream denies delete sharing.
    try {
        iiFileProvider::native::ReadFile stream(manifest);
        if (stream.size() == 0 || stream.size() > 16384) return false;
        const auto text = stream.read(static_cast<std::int64_t>(stream.size()));
        if (text.size() != stream.size()) return false;
        return Manifest(text).matches(expected);
    } catch (const iiFileProvider::native::FileError &) { return false; }
}

std::vector<fs::directory_entry> entries(const fs::path& directory, Report& report) {
    std::vector<fs::directory_entry> result;
    std::error_code ec;
    for (fs::directory_iterator iterator(directory, ec), end; !ec && iterator != end; iterator.increment(ec)) result.push_back(*iterator);
    if (ec) report.errors.push_back(directory.string() + ": " + ec.message());
    return result;
}
bool regular(const fs::directory_entry& entry) {
    std::error_code ec; const auto status = entry.symlink_status(ec);
    if (ec || !fs::is_regular_file(status)) return false;
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(entry.path().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
    return true;
}
bool temporaryName(const fs::path& path) {
    const auto name = path.filename().string();
    for (auto prefix : {".society-import-", ".society-write-", ".iifile-stage-", ".society-install-", ".society-extract-"})
        if (name.starts_with(prefix) && name.size() > std::char_traits<char>::length(prefix)) return true;
    return false;
}
bool transferName(const fs::path& path) {
    const auto name = path.filename().string();
    if (!name.ends_with(".part")) return false;
    const std::string_view stem(name.data(), name.size() - 5);
    if (stem.starts_with("provider-")) return uuid(stem.substr(9));
    return stem.size() == 64 && std::all_of(stem.begin(), stem.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool capabilityName(const fs::path& path) {
    const auto name = path.filename().string();
    if (!name.starts_with("capability-")) return false;
    auto suffix = std::string_view(name).substr(11);
    if (suffix.ends_with(".link")) suffix.remove_suffix(5);
    return uuid(suffix);
}

#ifndef _WIN32
// Open every component without following a link. Deletion is relative to this
// descriptor, so replacing an ancestor with a symlink cannot redirect unlink.
int openDirectory(const fs::path& path) {
    int handle = ::open(path.root_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (handle < 0) return -1;
    for (const auto& part : path.relative_path()) {
        const auto next = ::openat(handle, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(handle); handle = next; if (handle < 0) return -1;
    }
    return handle;
}
#endif
constexpr std::array<unsigned char, 8> finderMagic{0, 0, 0, 1, 'B', 'u', 'd', '1'};
#ifdef _WIN32
bool finderHeader(HANDLE file) {
    std::array<unsigned char, 8> header{}; DWORD size = 0;
    return ReadFile(file, header.data(), static_cast<DWORD>(header.size()), &size, nullptr)
        && size == header.size() && header == finderMagic;
}
#else
bool finderHeader(int file) {
    std::array<unsigned char, 8> header{}; ssize_t size;
    do { size = ::pread(file, header.data(), header.size(), 0); } while (size < 0 && errno == EINTR);
    return size == static_cast<ssize_t>(header.size()) && header == finderMagic;
}
#endif
bool finderCacheFile(const fs::path& path) {
    if (!direct(path, false)) return false;
#ifdef _WIN32
    const auto file = iiFileProvider::native::createFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid = GetFileInformationByHandle(file, &info)
        && !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) && finderHeader(file);
    CloseHandle(file); return valid;
#else
    const auto parent = openDirectory(path.parent_path()); if (parent < 0) return false;
    const auto file = ::openat(parent, path.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    ::close(parent); if (file < 0) return false;
    struct stat info{}; const bool valid = ::fstat(file, &info) == 0 && S_ISREG(info.st_mode) && finderHeader(file);
    ::close(file); return valid;
#endif
}
enum class Removal { Temporary, Log, Cache };
bool removeFile(const fs::path& path, Report& report, Removal kind) {
    std::uint64_t bytes = 0;
#ifdef _WIN32
    if (!direct(path, false)) return false;
    const auto access = DELETE | FILE_READ_ATTRIBUTES | (kind == Removal::Cache ? GENERIC_READ : 0);
    const auto handle = iiFileProvider::native::createFile(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { report.errors.push_back("Cannot open temporary file: " + path.string()); return false; }
    BY_HANDLE_FILE_INFORMATION info{};
    bool removed = false;
    if (GetFileInformationByHandle(handle, &info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        if (kind == Removal::Cache && !finderHeader(handle)) { CloseHandle(handle); return false; }
        bytes = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        FILE_DISPOSITION_INFO disposition{TRUE}; removed = SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
    }
    CloseHandle(handle);
    if (!removed) { report.errors.push_back("Cannot remove temporary file: " + path.string()); return false; }
#else
    const auto parent = openDirectory(path.parent_path());
    if (parent < 0) { report.errors.push_back("Cannot open temporary parent: " + path.parent_path().string()); return false; }
    struct stat info{};
    const auto name = path.filename();
    if (::fstatat(parent, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(info.st_mode)) { ::close(parent); return false; }
    if (kind == Removal::Cache) {
        const auto file = ::openat(parent, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        struct stat opened{};
        const bool valid = file >= 0 && ::fstat(file, &opened) == 0 && S_ISREG(opened.st_mode)
            && opened.st_dev == info.st_dev && opened.st_ino == info.st_ino && finderHeader(file);
        if (file >= 0) ::close(file);
        if (!valid) { ::close(parent); return false; }
        info = opened;
    }
    bytes = static_cast<std::uint64_t>(info.st_size);
    const auto removed = ::unlinkat(parent, name.c_str(), 0) == 0;
    ::close(parent);
    if (!removed) { report.errors.push_back("Cannot remove temporary file: " + path.string()); return false; }
#endif
    report.bytesRemoved += bytes;
    if (kind == Removal::Log) ++report.logsRemoved;
    else if (kind == Removal::Cache) ++report.cacheRemoved;
    else ++report.tempRemoved;
    return true;
}
bool removeDirectory(const fs::path& path) {
#ifdef _WIN32
    return direct(path, true) && RemoveDirectoryW(path.c_str());
#else
    const auto parent = openDirectory(path.parent_path()); if (parent < 0) return false;
    const bool removed = ::unlinkat(parent, path.filename().c_str(), AT_REMOVEDIR) == 0; ::close(parent); return removed;
#endif
}

class Scan final {
    const fs::path root;
    const std::string expected;
    const Identity original;
    Report& report;
    bool current(const fs::path& parent) {
        if (validRoot(root, expected, original) && direct(parent, true)) return true;
        report.errors.push_back("Storage identity or direct parent changed: " + parent.string()); return false;
    }
    template<class Predicate> void files(const fs::path& parent, Predicate predicate) {
        if (!direct(parent, true)) return;
        const auto candidates = entries(parent, report);
        if (std::none_of(candidates.begin(), candidates.end(), [&](const auto& entry) { return regular(entry) && predicate(entry.path()); })) return;
        Lease lease(parent, Lease::Mode::TryExclusive);
        if (!lease.locked()) { ++report.skippedBusy; return; }
        if (!current(parent)) return;
        for (const auto& entry : entries(parent, report)) if (regular(entry) && predicate(entry.path())) {
            if (!current(parent)) return; removeFile(entry.path(), report, Removal::Temporary);
        }
    }
    void snapshots(const fs::path& parent) {
        if (!direct(parent, true)) return;
        const auto snapshot = [](const auto& entry) { return entry.path().filename().string().starts_with("snapshot-") && direct(entry.path(), true); };
        const auto candidates = entries(parent, report);
        if (std::none_of(candidates.begin(), candidates.end(), snapshot)) return;
        Lease lease(parent, Lease::Mode::TryExclusive);
        if (!lease.locked()) { ++report.skippedBusy; return; }
        if (!current(parent)) return;
        for (const auto& entry : entries(parent, report)) {
            if (!snapshot(entry) || !current(entry.path())) continue;
            const auto contents = entries(entry.path(), report);
            // ObjectSource owns only a single payload. Unknown or redirected
            // contents cause the complete directory to be preserved.
            if (!std::all_of(contents.begin(), contents.end(), [](const auto& file) { return regular(file) && file.path().filename() == "payload"; })) continue;
            for (const auto& file : contents) {
                if (!current(entry.path())) return; removeFile(file.path(), report, Removal::Temporary);
            }
            if (current(entry.path())) removeDirectory(entry.path());
        }
    }
    void logs(const fs::path& parent) {
        if (!direct(parent, true)) return;
        const auto logsOnly = [](const auto& entry) { return regular(entry) && entry.path().extension() == ".log"; };
        const auto candidates = entries(parent, report);
        if (std::count_if(candidates.begin(), candidates.end(), logsOnly) < 2) return;
        Lease lease(parent, Lease::Mode::TryExclusive);
        if (!lease.locked()) { ++report.skippedBusy; return; }
        if (!current(parent)) return;
        std::vector<std::pair<fs::file_time_type, fs::path>> owned;
        for (const auto& entry : entries(parent, report)) if (logsOnly(entry)) {
            std::error_code ec; const auto time = entry.last_write_time(ec);
            if (ec) { report.errors.push_back(entry.path().string() + ": " + ec.message()); return; }
            owned.emplace_back(time, entry.path());
        }
        std::sort(owned.begin(), owned.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second.filename() > b.second.filename(); });
        for (std::size_t i = 1; i < owned.size(); ++i) { if (!current(parent)) return; removeFile(owned[i].second, report, Removal::Log); }
    }
    void orphanJobs(const fs::path& parent, bool privateJobs) {
        if (!direct(parent, true)) return;
        const auto owned = [privateJobs](const fs::path& path) {
            const auto name = path.filename().string();
            if (privateJobs) {
                for (auto prefix : {".society-model-merge-", ".society-model-package-"})
                    if (name.starts_with(prefix) && uuid(std::string_view(name).substr(std::char_traits<char>::length(prefix)))) return true;
            }
            constexpr auto prefix = ".iild-merge-";
            if (!name.starts_with(prefix) || name.size() != std::char_traits<char>::length(prefix) + 8) return false;
            return std::all_of(name.begin() + std::char_traits<char>::length(prefix), name.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            });
        };
        const auto candidates = entries(parent, report);
        if (std::none_of(candidates.begin(), candidates.end(), [&](const auto& entry) { return owned(entry.path()) && direct(entry.path(), true); })) return;
        Lease lease(parent, Lease::Mode::TryExclusive);
        if (!lease.locked()) { ++report.skippedBusy; return; }
        for (const auto& job : entries(parent, report)) {
            if (!owned(job.path()) || !direct(job.path(), true) || !current(parent)) continue;
            std::vector<fs::path> directories{job.path()}, files;
            bool safe = true;
            for (std::size_t i = 0; safe && i < directories.size(); ++i) {
                for (const auto& entry : entries(directories[i], report)) {
                    if (regular(entry)) files.push_back(entry.path());
                    else if (direct(entry.path(), true)) directories.push_back(entry.path());
                    else { safe = false; break; }
                }
            }
            if (!safe) continue; // A redirected or unknown entry preserves the whole workspace.
            for (const auto& file : files) {
                if (!current(file.parent_path())) return;
                removeFile(file, report, Removal::Temporary);
            }
            for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
                if (!current(it->parent_path())) return;
                removeDirectory(*it);
            }
        }
    }
    void finderCache(const fs::path& parent) {
        const auto path = parent / ".DS_Store";
        if (!finderCacheFile(path)) return;
        Lease lease(parent, Lease::Mode::TryExclusive);
        if (!lease.locked()) { ++report.skippedBusy; return; }
        if (current(parent) && finderCacheFile(path)) removeFile(path, report, Removal::Cache);
    }
    void section(const fs::path& path) {
        if (!direct(path, true)) return;
        std::vector<fs::path> pending{path};
        while (!pending.empty()) {
            const auto parent = std::move(pending.back()); pending.pop_back();
            if (!direct(parent, true)) continue;
            orphanJobs(parent, false);
            files(parent, temporaryName);
            for (const auto& entry : entries(parent, report)) {
                std::error_code ec;
                if (fs::is_directory(entry.symlink_status(ec)) && !ec && direct(entry.path(), true)) pending.push_back(entry.path());
            }
        }
    }
public:
    Scan(fs::path path, std::string id, Identity node, Report& result) : root(std::move(path)), expected(std::move(id)), original(node), report(result) {}
    void run() {
        finderCache(root);
        finderCache(root / "Models");
        files(root, temporaryName);
        for (auto name : sections) section(root / name);
        snapshots(root / ".society-objects/staging");
        files(root / ".society-sync", capabilityName);
        files(root / ".society-sync/transfers", transferName);
        files(root / ".society-sync/uploads", [](const auto& path) { return uuid(path.filename().string()); });
        orphanJobs(root / ".society-runtime/tmp", true);
        for (auto name : {".society-maintenance/logs", ".society-sync/logs", ".society-logs", ".society-runtime/logs"}) logs(root / name);
    }
};

class Monitor final {
    struct Registration { std::string id; std::chrono::seconds interval; std::chrono::steady_clock::time_point next; };
    std::mutex mutex;
    std::condition_variable changed;
    std::map<fs::path, Registration> registrations;
    std::thread worker;
    bool stopping = false;
    std::uint64_t revision = 0;
    void work() {
        std::unique_lock lock(mutex);
        while (!stopping) {
            const auto now = std::chrono::steady_clock::now();
            std::vector<std::pair<fs::path, std::string>> due;
            auto next = std::chrono::steady_clock::time_point::max();
            for (auto& [path, registration] : registrations) {
                if (registration.next <= now) { due.emplace_back(path, registration.id); registration.next = now + registration.interval; }
                next = std::min(next, registration.next);
            }
            if (!due.empty()) {
                lock.unlock();
                for (const auto& [path, id] : due) StorageMaintenance::runOnce(path, id);
                lock.lock(); continue;
            }
            const auto previous = revision;
            changed.wait_until(lock, next, [&] { return stopping || revision != previous; });
        }
    }
public:
    ~Monitor() {
        { std::lock_guard lock(mutex); stopping = true; }
        changed.notify_all(); if (worker.joinable()) worker.join();
    }
    void add(const fs::path& root, std::string id, std::chrono::seconds interval) {
        if (!validRoot(root, id)) return;
        interval = std::max(interval, std::chrono::seconds(1));
        std::lock_guard lock(mutex);
        if (const auto found = registrations.find(root); found != registrations.end()) {
            if (found->second.id == id) return;
            found->second = {std::move(id), interval, std::chrono::steady_clock::now()};
        } else {
            // Stale roots are pruned before accepting more registrations. The
            // hard bound prevents repeated opens of arbitrary paths from growing
            // a process-global registry indefinitely.
            if (registrations.size() >= 128) {
                for (auto it = registrations.begin(); it != registrations.end();) {
                    if (!validRoot(it->first, it->second.id)) it = registrations.erase(it); else ++it;
                }
                if (registrations.size() >= 128) return;
            }
            registrations.emplace(root, Registration{std::move(id), interval, std::chrono::steady_clock::now()});
        }
        std::vector<fs::path> content;
        for (auto name : sections) if (std::string_view(name) != "Deleted") content.emplace_back(name);
        const auto expected = registrations.at(root).id;
        try { iiFileProvider::native::Presentation::watch(root, content, std::chrono::seconds(2),
            [root, expected] { return validRoot(root, expected); }); }
        catch (const std::exception &error) { std::clog << "iiSocietyContainer: presentation intake registration failed: " << error.what() << '\n'; }
        ++revision;
        if (!worker.joinable()) worker = std::thread([this] { work(); });
        changed.notify_all();
    }
};
}

StorageMaintenance::Report StorageMaintenance::runOnce(const fs::path& root, std::string_view identifier) {
    Report report;
    try {
        if (!validRoot(root, identifier)) { report.errors.emplace_back("Maintenance requires an ordinary direct SocietyDrive directory with the expected schema-1 identity."); return report; }
        const auto node = identity(root);
        if (!node) { report.errors.emplace_back("Cannot identify storage directory."); return report; }
        Scan(root, std::string(identifier), *node, report).run();
    } catch (const std::exception& error) { report.errors.push_back(error.what()); }
    return report;
}
void StorageMaintenance::monitor(const fs::path& root, std::string identifier, std::chrono::seconds interval) {
    static Monitor monitor;
    monitor.add(root, std::move(identifier), interval);
}
}
