#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef __MINGW32__
// Dokan's public headers expect the Windows SDK NTSTATUS typedef. MinGW's
// ntstatus.h supplies constants only; importing winternl.h duplicates fileinfo.h.
typedef long NTSTATUS;
#endif
#include <dokan.h>
#include "platform/desktop/NativeMount.h"
#include <QDir>
#include <QSettings>
#include <QCryptographicHash>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>
#include <iterator>

namespace iiSocietyContainer {
namespace {
struct FileHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::mutex mutex;
    ~FileHandle() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
class DokanMount final : public NativeMount {
public:
    FilesView view;
    QString point;
    std::wstring nativePoint;
    DOKAN_HANDLE instance = nullptr;
    DOKAN_OPTIONS options{};
    DOKAN_OPERATIONS operations{};
    DokanMount(FilesView input, QString mountPoint)
        : view(std::move(input)), point(std::move(mountPoint)), nativePoint(point.toStdWString()) {}
    ~DokanMount() override { if (instance) DokanCloseHandle(instance); }
    QString path() const override { return point + '\\'; }
    bool isRunning() const override { return instance && DokanIsFileSystemRunning(instance); }
    static DokanMount &self(PDOKAN_FILE_INFO info) { return *reinterpret_cast<DokanMount *>(info->DokanOptions->GlobalContext); }
    static QString relative(LPCWSTR name)
    {
        auto path = QString::fromWCharArray(name);
        path.replace('\\', '/');
        return path.startsWith('/') ? path.mid(1) : path;
    }
    QString source(LPCWSTR name, bool missing = false) const { return view.resolve(relative(name), missing); }
};
NTSTATUS lastError() { return DokanNtStatusFromWin32(GetLastError()); }
std::wstring native(const QString &path)
{
    auto result = QDir::toNativeSeparators(path).toStdWString();
    return result.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + result.substr(2) : L"\\\\?\\" + result;
}
FileHandle *context(PDOKAN_FILE_INFO info) { return reinterpret_cast<FileHandle *>(info->Context); }
bool isRoot(LPCWSTR name) { return DokanMount::relative(name).isEmpty(); }
bool regularHandle(HANDLE handle, const QString &expected)
{
    BY_HANDLE_FILE_INFORMATION data{};
    if (!GetFileInformationByHandle(handle, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    std::vector<wchar_t> path(32768);
    const auto length = GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED);
    if (!length || length >= path.size()) return false;
    return QString::fromWCharArray(path.data()).compare(QString::fromStdWString(native(expected)), Qt::CaseInsensitive) == 0;
}
NTSTATUS DOKAN_CALLBACK createEntry(LPCWSTR name, PDOKAN_IO_SECURITY_CONTEXT, ACCESS_MASK access,
    ULONG attributes, ULONG sharing, ULONG disposition, ULONG options, PDOKAN_FILE_INFO info)
{
    const auto source = DokanMount::self(info).source(name, true);
    if (source.isEmpty()) return STATUS_OBJECT_NAME_NOT_FOUND;
    ACCESS_MASK requested; DWORD flags, creation;
    DokanMapKernelToUserCreateFileFlags(access, attributes, options, disposition, &requested, &flags, &creation);
    if (isRoot(name) && ((access & DELETE) || (options & FILE_DELETE_ON_CLOSE) || creation == CREATE_ALWAYS
                         || creation == CREATE_NEW || creation == TRUNCATE_EXISTING)) return STATUS_ACCESS_DENIED;
    auto diskPath = native(source);
    DWORD existingAttributes = GetFileAttributesW(diskPath.c_str());
    const bool existed = existingAttributes != INVALID_FILE_ATTRIBUTES;
    if (existed && (existingAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return STATUS_ACCESS_DENIED;
    const bool directory = (options & FILE_DIRECTORY_FILE) || (existed && (existingAttributes & FILE_ATTRIBUTE_DIRECTORY));
    if (directory && (options & FILE_NON_DIRECTORY_FILE)) return STATUS_FILE_IS_A_DIRECTORY;
    if (directory) {
        if (!existed && (creation == CREATE_NEW || creation == OPEN_ALWAYS)) {
            if (!CreateDirectoryW(diskPath.c_str(), nullptr)) return lastError();
        } else if (existed && creation == CREATE_NEW) return STATUS_OBJECT_NAME_COLLISION;
        else if (creation == CREATE_ALWAYS || creation == TRUNCATE_EXISTING) return STATUS_ACCESS_DENIED;
        creation = OPEN_EXISTING;
        info->IsDirectory = TRUE;
        flags |= FILE_FLAG_BACKUP_SEMANTICS;
    }
    // Reparse data and alternate streams are never served by this projection.
    flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    auto handle = std::make_unique<FileHandle>();
    handle->handle = CreateFileW(diskPath.c_str(), requested, sharing, nullptr, creation, flags, nullptr);
    if (handle->handle == INVALID_HANDLE_VALUE) return lastError();
    if (!regularHandle(handle->handle, source)) return STATUS_ACCESS_DENIED;
    info->Context = reinterpret_cast<ULONG64>(handle.release());
    return existed && (creation == OPEN_ALWAYS || creation == CREATE_ALWAYS) ? STATUS_OBJECT_NAME_COLLISION : STATUS_SUCCESS;
}
void DOKAN_CALLBACK cleanupEntry(LPCWSTR, PDOKAN_FILE_INFO info)
{
    // Delete was validated and marked on this exact handle by DeleteFile/Directory.
    delete context(info);
    info->Context = 0;
}
void DOKAN_CALLBACK closeEntry(LPCWSTR name, PDOKAN_FILE_INFO info) { cleanupEntry(name, info); }
NTSTATUS DOKAN_CALLBACK readEntry(LPCWSTR name, LPVOID buffer, DWORD length, LPDWORD read,
    LONGLONG offset, PDOKAN_FILE_INFO info)
{
    *read = 0;
    if (!DokanMount::self(info).view.isValid()) return STATUS_DEVICE_NOT_READY;
    auto *file = context(info);
    std::unique_ptr<FileHandle> reopened;
    if (!file) {
        const auto source = DokanMount::self(info).source(name);
        if (source.isEmpty()) return STATUS_OBJECT_NAME_NOT_FOUND;
        reopened = std::make_unique<FileHandle>();
        reopened->handle = CreateFileW(native(source).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (reopened->handle == INVALID_HANDLE_VALUE) return lastError();
        if (!regularHandle(reopened->handle, source)) return STATUS_ACCESS_DENIED;
        file = reopened.get();
    }
    std::lock_guard lock(file->mutex);
    LARGE_INTEGER position; position.QuadPart = offset;
    if (!SetFilePointerEx(file->handle, position, nullptr, FILE_BEGIN)) return lastError();
    return ReadFile(file->handle, buffer, length, read, nullptr) ? STATUS_SUCCESS : lastError();
}
NTSTATUS DOKAN_CALLBACK writeEntry(LPCWSTR name, LPCVOID buffer, DWORD length, LPDWORD written,
    LONGLONG offset, PDOKAN_FILE_INFO info)
{
    *written = 0;
    if (!DokanMount::self(info).view.isValid() || isRoot(name)) return STATUS_ACCESS_DENIED;
    auto *file = context(info);
    std::unique_ptr<FileHandle> reopened;
    if (!file) {
        const auto source = DokanMount::self(info).source(name);
        if (source.isEmpty()) return STATUS_OBJECT_NAME_NOT_FOUND;
        reopened = std::make_unique<FileHandle>();
        reopened->handle = CreateFileW(native(source).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (reopened->handle == INVALID_HANDLE_VALUE) return lastError();
        if (!regularHandle(reopened->handle, source)) return STATUS_ACCESS_DENIED;
        file = reopened.get();
    }
    std::lock_guard lock(file->mutex);
    LARGE_INTEGER position; position.QuadPart = offset;
    if (info->WriteToEndOfFile) position.QuadPart = 0;
    if (info->PagingIo) {
        LARGE_INTEGER size;
        if (!GetFileSizeEx(file->handle, &size)) return lastError();
        if (offset >= size.QuadPart) return STATUS_SUCCESS;
        length = static_cast<DWORD>(std::min<LONGLONG>(length, size.QuadPart - offset));
    }
    if (!SetFilePointerEx(file->handle, position, nullptr, info->WriteToEndOfFile ? FILE_END : FILE_BEGIN)) return lastError();
    return WriteFile(file->handle, buffer, length, written, nullptr) ? STATUS_SUCCESS : lastError();
}
NTSTATUS DOKAN_CALLBACK flushEntry(LPCWSTR, PDOKAN_FILE_INFO info)
{
    auto *file = context(info);
    return !file || FlushFileBuffers(file->handle) ? STATUS_SUCCESS : lastError();
}
NTSTATUS DOKAN_CALLBACK information(LPCWSTR name, LPBY_HANDLE_FILE_INFORMATION result, PDOKAN_FILE_INFO info)
{
    const auto source = DokanMount::self(info).source(name);
    if (source.isEmpty()) return STATUS_OBJECT_NAME_NOT_FOUND;
    auto *file = context(info);
    FileHandle temporary;
    if (!file) {
        temporary.handle = CreateFileW(native(source).c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (temporary.handle == INVALID_HANDLE_VALUE) return lastError();
        file = &temporary;
    }
    return regularHandle(file->handle, source) && GetFileInformationByHandle(file->handle, result) ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
}
NTSTATUS DOKAN_CALLBACK listEntries(LPCWSTR name, PFillFindData fill, PDOKAN_FILE_INFO info)
{
    QString error;
    const auto entries = DokanMount::self(info).view.entries(DokanMount::relative(name), &error);
    if (!error.isEmpty()) return STATUS_OBJECT_NAME_NOT_FOUND;
    for (const auto &entry : entries) {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(native(entry.absoluteFilePath()).c_str(), GetFileExInfoStandard, &attributes)) continue;
        if (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        WIN32_FIND_DATAW found{};
        found.dwFileAttributes = attributes.dwFileAttributes;
        found.ftCreationTime = attributes.ftCreationTime;
        found.ftLastAccessTime = attributes.ftLastAccessTime;
        found.ftLastWriteTime = attributes.ftLastWriteTime;
        found.nFileSizeHigh = attributes.nFileSizeHigh;
        found.nFileSizeLow = attributes.nFileSizeLow;
        const auto fileName = entry.fileName().toStdWString();
        if (fileName.size() >= std::size(found.cFileName)) continue;
        std::copy(fileName.begin(), fileName.end(), found.cFileName);
        if (fill(&found, info)) break;
    }
    return STATUS_SUCCESS;
}
NTSTATUS markDeletion(LPCWSTR name, PDOKAN_FILE_INFO info, bool directory)
{
    const auto source = DokanMount::self(info).source(name);
    if (source.isEmpty() || isRoot(name)) return STATUS_ACCESS_DENIED;
    if (QFileInfo(source).isDir() != directory) return directory ? STATUS_NOT_A_DIRECTORY : STATUS_FILE_IS_A_DIRECTORY;
    if (directory && !QDir(source).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) return STATUS_DIRECTORY_NOT_EMPTY;
    auto *file = context(info);
    if (!file) return STATUS_INVALID_HANDLE;
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = info->DeletePending != FALSE;
    return SetFileInformationByHandle(file->handle, FileDispositionInfo, &disposition, sizeof(disposition)) ? STATUS_SUCCESS : lastError();
}
NTSTATUS DOKAN_CALLBACK deleteFile(LPCWSTR name, PDOKAN_FILE_INFO info) { return markDeletion(name, info, false); }
NTSTATUS DOKAN_CALLBACK deleteDirectory(LPCWSTR name, PDOKAN_FILE_INFO info) { return markDeletion(name, info, true); }
NTSTATUS DOKAN_CALLBACK moveEntry(LPCWSTR from, LPCWSTR to, BOOL replace, PDOKAN_FILE_INFO info)
{
    const auto source = DokanMount::self(info).source(from);
    const auto target = DokanMount::self(info).source(to, true);
    if (source.isEmpty() || target.isEmpty() || isRoot(from) || isRoot(to)) return STATUS_ACCESS_DENIED;
    auto *file = context(info);
    if (!file) return STATUS_INVALID_HANDLE;
    const auto destination = native(target);
    std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + destination.size() * sizeof(wchar_t), 0);
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    rename->ReplaceIfExists = replace != FALSE;
    rename->FileNameLength = static_cast<DWORD>(destination.size() * sizeof(wchar_t));
    std::copy(destination.begin(), destination.end(), rename->FileName);
    return SetFileInformationByHandle(file->handle, FileRenameInfo, rename, static_cast<DWORD>(storage.size())) ? STATUS_SUCCESS : lastError();
}
NTSTATUS DOKAN_CALLBACK setLength(LPCWSTR name, LONGLONG size, PDOKAN_FILE_INFO info)
{
    auto *file = context(info);
    if (!file || isRoot(name) || !DokanMount::self(info).view.isValid()) return STATUS_ACCESS_DENIED;
    std::lock_guard lock(file->mutex);
    LARGE_INTEGER position; position.QuadPart = size;
    return SetFilePointerEx(file->handle, position, nullptr, FILE_BEGIN) && SetEndOfFile(file->handle) ? STATUS_SUCCESS : lastError();
}
NTSTATUS DOKAN_CALLBACK setAllocation(LPCWSTR name, LONGLONG size, PDOKAN_FILE_INFO info)
{
    auto *file = context(info);
    if (!file || isRoot(name) || !DokanMount::self(info).view.isValid()) return STATUS_ACCESS_DENIED;
    FILE_ALLOCATION_INFO allocation{}; allocation.AllocationSize.QuadPart = size;
    return SetFileInformationByHandle(file->handle, FileAllocationInfo, &allocation, sizeof(allocation)) ? STATUS_SUCCESS : lastError();
}
NTSTATUS DOKAN_CALLBACK setAttributes(LPCWSTR name, DWORD attributes, PDOKAN_FILE_INFO info)
{
    const auto source = DokanMount::self(info).source(name);
    if (source.isEmpty() || isRoot(name) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return STATUS_ACCESS_DENIED;
    return !attributes || SetFileAttributesW(native(source).c_str(), attributes) ? STATUS_SUCCESS : lastError();
}
NTSTATUS DOKAN_CALLBACK setTimes(LPCWSTR name, const FILETIME *created, const FILETIME *accessed,
    const FILETIME *written, PDOKAN_FILE_INFO info)
{
    auto *file = context(info);
    if (!file || isRoot(name) || !DokanMount::self(info).view.isValid()) return STATUS_ACCESS_DENIED;
    return SetFileTime(file->handle, created, accessed, written) ? STATUS_SUCCESS : lastError();
}
NTSTATUS DOKAN_CALLBACK diskSpace(PULONGLONG available, PULONGLONG total, PULONGLONG free, PDOKAN_FILE_INFO info)
{
    ULARGE_INTEGER a{}, t{}, f{};
    if (!GetDiskFreeSpaceExW(native(DokanMount::self(info).view.rootPath()).c_str(), &a, &t, &f)) return lastError();
    *available = a.QuadPart; *total = t.QuadPart; *free = f.QuadPart;
    return STATUS_SUCCESS;
}
NTSTATUS DOKAN_CALLBACK volumeInfo(LPWSTR label, DWORD labelSize, LPDWORD serial, LPDWORD componentLength,
    LPDWORD flags, LPWSTR fileSystem, DWORD fileSystemSize, PDOKAN_FILE_INFO info)
{
    if (labelSize < 8 || fileSystemSize < 8) return STATUS_BUFFER_TOO_SMALL;
    wcscpy_s(label, labelSize, L"Society");
    wcscpy_s(fileSystem, fileSystemSize, L"Society");
    const auto digest = QCryptographicHash::hash(DokanMount::self(info).view.drive().identifier().toUtf8(), QCryptographicHash::Sha256);
    memcpy(serial, digest.constData(), sizeof(*serial));
    *componentLength = 255;
    *flags = FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK;
    return STATUS_SUCCESS;
}
}
QString defaultMountPoint(const QString &)
{
    const DWORD used = GetLogicalDrives();
    const QString letters = "STUVWXYZDEFGHIJKLMNOPQR";
    for (const auto letter : letters)
        if (!(used & (1u << (letter.unicode() - 'A')))) return QString(letter) + ':';
    return {};
}
std::unique_ptr<NativeMount> mountFiles(FilesView view, const QString &point, QString *error)
{
    static std::once_flag initialized;
    std::call_once(initialized, [] { DokanInit(); });
    if (DokanDriverVersion() < DOKAN_MINIMUM_COMPATIBLE_VERSION) {
        if (error) *error = "Install the signed Dokan 2 driver before connecting Society.";
        return {};
    }
    if (point.size() != 2 || point[1] != ':' || point[0] < 'D' || point[0] > 'Z'
        || (GetLogicalDrives() & (1u << (point[0].unicode() - 'A')))) {
        if (error) *error = "No unused drive letter is available.";
        return {};
    }
    auto mount = std::make_unique<DokanMount>(std::move(view), point);
    auto &op = mount->operations;
    op.ZwCreateFile = createEntry; op.Cleanup = cleanupEntry; op.CloseFile = closeEntry;
    op.ReadFile = readEntry; op.WriteFile = writeEntry; op.FlushFileBuffers = flushEntry;
    op.GetFileInformation = information; op.FindFiles = listEntries;
    op.DeleteFile = deleteFile; op.DeleteDirectory = deleteDirectory; op.MoveFile = moveEntry;
    op.SetEndOfFile = setLength; op.SetAllocationSize = setAllocation;
    op.SetFileAttributes = setAttributes; op.SetFileTime = setTimes;
    op.GetDiskFreeSpace = diskSpace; op.GetVolumeInformation = volumeInfo;
    mount->options.Version = DOKAN_VERSION;
    mount->options.Options = DOKAN_OPTION_CURRENT_SESSION;
    mount->options.GlobalContext = reinterpret_cast<ULONG64>(mount.get());
    mount->options.MountPoint = mount->nativePoint.c_str();
    const int status = DokanCreateFileSystem(&mount->options, &mount->operations, &mount->instance);
    if (status != DOKAN_SUCCESS) {
        if (error) *error = QString("Dokan could not mount Society Files (error %1).").arg(status);
        return {};
    }
    return mount;
}
bool installMountAutostart(const QString &executable, QString *error)
{
    QSettings settings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    // Retain the registration key so existing users do not get a second daemon.
    settings.setValue("Society Container", '"' + QDir::toNativeSeparators(executable) + "\" serve");
    settings.sync();
    if (settings.status() != QSettings::NoError) { if (error) *error = "Could not register Society for this login session."; return false; }
    return true;
}
}
