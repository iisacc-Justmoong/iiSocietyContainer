#include "FileTree.h"
#include <iiFileProviderPresentation.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace iiSocietyContainer {
namespace {
namespace fs = std::filesystem;

template<class T> FileTree::Result<T> failed(std::error_code error)
{
    return {std::nullopt, error};
}
std::error_code invalidPath() { return std::make_error_code(std::errc::invalid_argument); }

bool validRelative(const fs::path& relative)
{
    if (relative.empty() || relative == ".") return true;
    const auto& text = relative.native();
    using Char = fs::path::value_type;
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()
        || text.find(Char{}) != text.npos
#ifndef _WIN32
        || text.find(Char('\\')) != text.npos
#endif
        || text.find(Char(':')) != text.npos
        || text.find(std::basic_string<Char>(2, Char('/'))) != text.npos
#ifdef _WIN32
        || text.find(std::basic_string<Char>(2, Char('\\'))) != text.npos
#endif
        ) return false;
    for (const auto& component : relative)
        if (component.empty() || component == "." || component == ".." || iiFileProvider::native::Presentation::isControlFile(component)) return false;
    return true;
}

FileTree::EntryResult metadata(const fs::path& root, const fs::path& relative)
{
    if (!validRelative(relative)) return failed<FileTree::Entry>(invalidPath());
    const auto key = relative.empty() || relative == "." ? fs::path(".") : relative;
    auto current = root;
    std::error_code error;
    // Pin the root and inspect every descendant before resolving its native
    // spelling. APFS may return a decomposed Unicode filename for a composed input.
    auto inspect = [&](bool rootEntry) {
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            error = std::make_error_code(std::errc::too_many_symbolic_link_levels); return false;
        }
#endif
        const auto status = fs::symlink_status(current, error);
        if (error) return false;
        if (fs::is_symlink(status)) { error = std::make_error_code(std::errc::too_many_symbolic_link_levels); return false; }
        const auto canonical = fs::canonical(current, error);
        if (error) return false;
        if (rootEntry ? canonical != current : canonical.parent_path() != current.parent_path()) {
            error = invalidPath(); return false;
        }
        current = canonical;
        return true;
    };
    if (!inspect(true)) return failed<FileTree::Entry>(error);
    if (!fs::is_directory(current, error))
        return failed<FileTree::Entry>(error ? error : std::make_error_code(std::errc::not_a_directory));
    if (key != ".") {
        for (const auto& component : key) {
            if (!fs::is_directory(current, error))
                return failed<FileTree::Entry>(error ? error : std::make_error_code(std::errc::not_a_directory));
            current /= component;
            if (!inspect(false)) return failed<FileTree::Entry>(error);
        }
    }
    const auto status = fs::status(current, error);
    if (error) return failed<FileTree::Entry>(error);
    if (!fs::is_regular_file(status) && !fs::is_directory(status))
        return failed<FileTree::Entry>(std::make_error_code(std::errc::operation_not_supported));
    FileTree::Entry result;
    result.path = current;
    result.relativePath = current.lexically_relative(root);
    result.parentPath = key == "." ? fs::path{} : result.relativePath.parent_path();
    if (key != "." && result.parentPath.empty()) result.parentPath = ".";
    result.kind = fs::is_directory(status) ? FileTree::Kind::Directory : FileTree::Kind::File;
    result.permissions = status.permissions();
    if (result.kind == FileTree::Kind::File) {
        result.size = fs::file_size(current, error);
        if (error) return failed<FileTree::Entry>(error);
    }
    result.lastModified = fs::last_write_time(current, error);
    if (error) return failed<FileTree::Entry>(error);
    return {std::move(result), {}};
}
}

const FileTree::Entry* FileTree::Entry::find(const std::filesystem::path& relative) const
{
    const auto key = relative.empty() ? std::filesystem::path(".") : relative;
    if (relativePath == key) return this;
    for (const auto& child : children)
        if (const auto* found = child.find(key)) return found;
    return nullptr;
}

FileTree::FileTree(const std::filesystem::path& root)
{
    if (root.empty() || root.native().find(std::filesystem::path::value_type{}) != root.native().npos) {
        m_error = invalidPath(); return;
    }
    m_root = std::filesystem::canonical(root, m_error);
    if (!m_error && !std::filesystem::is_directory(m_root, m_error))
        m_error = std::make_error_code(std::errc::not_a_directory);
    if (m_error) m_root.clear();
}

const std::filesystem::path& FileTree::rootPath() const { return m_root; }
bool FileTree::isValid() const { return bool(entry()); }
FileTree::EntryResult FileTree::entry(const std::filesystem::path& relative) const
{
    if (m_error) return failed<Entry>(m_error);
    return metadata(m_root, relative);
}
FileTree::PresentationResult FileTree::presentation(const std::filesystem::path& relative) const
{
    const auto file = entry(relative);
    if (!file) return failed<iiFileProvider::native::PresentationRecord>(file.error);
    if (file.value->kind != Kind::File)
        return failed<iiFileProvider::native::PresentationRecord>(std::make_error_code(std::errc::is_a_directory));
    try {
        auto record = iiFileProvider::native::Presentation::prepare(file.value->path);
        const auto after = entry(relative);
        const auto verified = iiFileProvider::native::Presentation::load(file.value->path);
        if (!after || after.value->path != file.value->path || after.value->size != record.details.size
            || !verified || verified->document != record.document)
            return failed<iiFileProvider::native::PresentationRecord>(std::make_error_code(std::errc::state_not_recoverable));
        return {std::move(record), {}};
    } catch (const std::filesystem::filesystem_error &e) {
        return failed<iiFileProvider::native::PresentationRecord>(e.code());
    } catch (...) {
        return failed<iiFileProvider::native::PresentationRecord>(std::make_error_code(std::errc::io_error));
    }
}

FileTree::ChildrenResult FileTree::children(const std::filesystem::path& relative, bool includeHidden) const
{
    return readChildren(relative, includeHidden, std::numeric_limits<std::size_t>::max());
}

FileTree::ChildrenResult FileTree::readChildren(const std::filesystem::path& relative,
                                               bool includeHidden, std::size_t limit) const
{
    const auto parent = entry(relative);
    if (!parent) return failed<std::vector<Entry>>(parent.error);
    if (parent.value->kind != Kind::Directory)
        return failed<std::vector<Entry>>(std::make_error_code(std::errc::not_a_directory));
    std::vector<Entry> result;
    std::error_code error;
    std::filesystem::directory_iterator iterator(parent.value->path, error), end;
    if (error) return failed<std::vector<Entry>>(error);
    for (; iterator != end; iterator.increment(error)) {
        if (error) return failed<std::vector<Entry>>(error);
        const auto name = iterator->path().filename();
        if (iiFileProvider::native::Presentation::isControlFile(name)) continue;
        if (!includeHidden && name.native().starts_with(std::filesystem::path(".").native())) continue;
        const auto status = iterator->symlink_status(error);
        if (error) return failed<std::vector<Entry>>(error);
        if (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status)) continue;
        const auto key = parent.value->relativePath == "." ? name : parent.value->relativePath / name;
        if (!validRelative(key)) continue;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(iterator->path().c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
#endif
        // The iterator supplies native filename spelling beneath the validated
        // parent. Do not reopen/recanonicalize every ancestor for every child.
        Entry child;
        child.path = iterator->path();
        child.relativePath = key;
        child.parentPath = parent.value->relativePath;
        child.kind = std::filesystem::is_directory(status) ? Kind::Directory : Kind::File;
        child.permissions = status.permissions();
        if (child.kind == Kind::File) {
            child.size = iterator->file_size(error);
            if (error) return failed<std::vector<Entry>>(error);
        }
        child.lastModified = iterator->last_write_time(error);
        if (error) return failed<std::vector<Entry>>(error);
        const auto after = std::filesystem::symlink_status(child.path, error);
        if (error) return failed<std::vector<Entry>>(error);
        if (std::filesystem::is_symlink(after)) continue;
        if (after.type() != status.type())
            return failed<std::vector<Entry>>(std::make_error_code(std::errc::state_not_recoverable));
        if (result.size() == limit)
            return failed<std::vector<Entry>>(std::make_error_code(std::errc::value_too_large));
        result.push_back(std::move(child));
    }
    if (error) return failed<std::vector<Entry>>(error);
    const auto checkedParent = entry(relative);
    if (!checkedParent || checkedParent.value->kind != Kind::Directory
        || checkedParent.value->path != parent.value->path)
        return failed<std::vector<Entry>>(checkedParent.error ? checkedParent.error
            : std::make_error_code(std::errc::state_not_recoverable));
    std::sort(result.begin(), result.end(), [](const Entry& a, const Entry& b) {
        if (a.kind != b.kind) return a.kind == Kind::Directory;
        return a.path.filename().native() < b.path.filename().native();
    });
    return {std::move(result), {}};
}

FileTree::EntryResult FileTree::snapshot(const std::filesystem::path& relative) const
{
    return snapshot(relative, Options{});
}
FileTree::EntryResult FileTree::snapshot(const std::filesystem::path& relative, const Options& options) const
{
    if (options.maxEntries == 0 || options.maxDepth > 256) return failed<Entry>(invalidPath());
    auto result = entry(relative);
    if (!result) return result;
    std::size_t count = 1;
    std::function<std::error_code(Entry&, std::size_t)> load = [&](Entry& node, std::size_t depth) -> std::error_code {
        if (node.kind != Kind::Directory || depth == options.maxDepth) return {};
        auto list = readChildren(node.relativePath, options.includeHidden, options.maxEntries - count);
        if (!list) return list.error;
        if (list.value->size() > options.maxEntries - count)
            return std::make_error_code(std::errc::value_too_large);
        count += list.value->size();
        node.children = std::move(*list.value);
        node.childrenLoaded = true;
        for (auto& child : node.children)
            if (const auto error = load(child, depth + 1)) return error;
        return {};
    };
    if (const auto error = load(*result.value, 0)) return failed<Entry>(error);
    return result;
}

} // namespace iiSocietyContainer
