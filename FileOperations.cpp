#include "FileOperations.h"
#include "StorageMap.h"
#include "DiskImage.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QScopeGuard>
#include <QUuid>
#if defined(Q_OS_DARWIN)
#include <stdio.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#elif defined(Q_OS_LINUX)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#ifdef Q_OS_MACOS
#include <copyfile.h>
#endif

namespace iiSocietyContainer {
namespace {
bool confined(const SocietyDrive &drive, const QString &path, bool section = false) {
    if (!drive.isValid() || !QFileInfo(path).isAbsolute() || path != QDir::cleanPath(path)) return false;
    const auto relative = drive.relativePath(path);
    if (relative.isEmpty()) return false;
    bool known = false;
    for (const auto s : allStoreSections()) known |= relative.section('/', 0, 0) == storeSectionName(s);
    if (!known || (!section && !relative.contains('/'))) return false;
    auto current = drive.resolvePath(relative.section('/', 0, 0));
    if (current.isEmpty() || !QFileInfo(current).isDir() || QFileInfo(current).isSymLink()
        || QFileInfo(current).canonicalFilePath() != current) return false;
    bool first = true;
    for (const auto &part : relative.split('/')) {
        if (part.isEmpty() || part == "." || part == ".." || part.contains('\\') || part.contains(':')
            || part.contains(QChar::Null) || part.startsWith(".society-") || part.startsWith(".iiserverhost-")) return false;
        if (first) { first = false; continue; }
        current += '/' + part; const QFileInfo info(current);
        if (info.isSymLink() || info.isJunction() || (info.exists() && info.canonicalFilePath() != current)) return false;
    }
    return true;
}
QString uniquePath(const QString &parent, const QString &name, bool duplicate) {
    const QFileInfo info(name); const auto suffix = info.suffix();
    const auto ext = suffix.isEmpty() ? QString() : '.' + suffix;
    const auto stem = ext.isEmpty() ? name : name.chopped(ext.size());
    auto result = QDir(parent).filePath(duplicate ? stem + " copy" + ext : name);
    for (int n = 2; QFileInfo::exists(result) || QFileInfo(result).isSymLink(); ++n) {
        if (n > 10000) return {};
        result = QDir(parent).filePath(stem + (duplicate ? " copy " : " ") + QString::number(n) + ext);
    }
    return result;
}
bool copyTree(const QString &source, const QString &destination, QString *error) {
    const QFileInfo before(source);
    if (before.isSymLink() || before.isJunction() || (!before.isFile() && !before.isDir())) {
        *error = "This item contains an unavailable file or a symbolic link."; return false;
    }
    if (before.isDir()) {
        if (!QDir().mkdir(destination)) { *error = "Could not create the copy."; return false; }
        const QDir directory(source);
        const auto entries = directory.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        if (!before.isReadable()) { *error = "The source folder cannot be read."; return false; }
        for (const auto &name : entries) {
            if (!copyTree(directory.filePath(name), QDir(destination).filePath(name), error)) return false;
        }
        if (entries != directory.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
            *error = "The folder changed while it was being copied. Try again."; return false;
        }
    } else {
        bool copied = false;
#ifdef Q_OS_MACOS
        // APFS clones avoid rereading multi-GB model weights. Other filesystems
        // transparently fall back to copyfile's ordinary data copy.
        copied = ::copyfile(QFile::encodeName(source).constData(), QFile::encodeName(destination).constData(),
                            nullptr, COPYFILE_ALL | COPYFILE_CLONE | COPYFILE_EXCL | COPYFILE_NOFOLLOW_SRC) == 0;
#else
        copied = QFile::copy(source, destination);
#endif
        if (!copied) { *error = "Could not copy this file. Check available space and access."; return false; }
    }
    const QFileInfo after(source);
    if (!after.exists() || after.isSymLink() || after.isDir() != before.isDir() || after.size() != before.size()
        || after.lastModified() != before.lastModified() || after.metadataChangeTime() != before.metadataChangeTime()) {
        *error = "The source changed while it was being copied. Try again."; return false;
    }
    return true;
}
bool removeTree(const QString &path) {
    return QFileInfo(path).isDir() && !QFileInfo(path).isSymLink() ? QDir(path).removeRecursively() : QFile::remove(path);
}
bool moveWithoutCopy(const QString &source, const QString &destination) {
    // QFile/QDir rename may fall back to copying file contents. Trash must be a
    // single, no-replace filesystem rename, including when another writer races.
#if defined(Q_OS_DARWIN)
    return ::renamex_np(QFile::encodeName(source).constData(), QFile::encodeName(destination).constData(), RENAME_EXCL) == 0;
#elif defined(Q_OS_WIN)
    return ::MoveFileW(reinterpret_cast<LPCWSTR>(source.utf16()), reinterpret_cast<LPCWSTR>(destination.utf16()));
#elif defined(Q_OS_LINUX) && defined(SYS_renameat2)
    return ::syscall(SYS_renameat2, AT_FDCWD, QFile::encodeName(source).constData(),
                     AT_FDCWD, QFile::encodeName(destination).constData(), 1 /* RENAME_NOREPLACE */) == 0;
#else
    Q_UNUSED(source); Q_UNUSED(destination);
    return false; // Never silently copy a model or overwrite a destination.
#endif
}
}
FileOperations::FileOperations(SocietyDrive drive) : m_drive(std::move(drive)) {}
std::optional<SocietyDrive> FileOperations::containingDrive(const QString &path) {
    if (!QFileInfo(path).isAbsolute()) return {};
    if (const auto root = DiskImage::containerRoot(path.toStdString())) return SocietyDrive::open(QString::fromStdString(root->string()));
    QString parent = QDir::cleanPath(path);
    while (parent != QDir::rootPath()) {
        if (QFileInfo::exists(QDir(parent).filePath(".society-drive.json"))) return SocietyDrive::open(parent);
        parent = QFileInfo(parent).absolutePath();
    }
    return {};
}
bool FileOperations::editable(const QString &path) const {
    return confined(m_drive, path);
}
FileOperations::Result FileOperations::perform(Action action, const QString &source, const QString &argument) const {
    auto failed = [](const QString &error) { return Result{{}, error}; };
    const bool importing = action == Action::Copy;
    const QFileInfo sourceInfo(source);
    if (importing ? (!sourceInfo.isAbsolute() || sourceInfo.canonicalFilePath() != source || sourceInfo.isSymLink() || sourceInfo.isJunction()) : !editable(source))
        return failed("This protected folder or redirected path cannot be changed.");
    const bool deleting = action == Action::Trash || action == Action::Remove;
    if (!QFileInfo::exists(source)) return failed(deleting
        ? "This item is not stored on this device. Delete it on the Society host."
        : "Download this item from the Society host before changing it.");
    // Deletion has no dependency on catalog residency, downloads, hashes, model
    // packages, or the replication lock. The indexer observes changes afterwards.
    if (action == Action::Remove) {
        if (!source.startsWith(m_drive.sectionPath(StoreSection::Deleted) + '/')) return failed("Only items in Deleted can be permanently removed.");
        return removeTree(source) ? Result{source, {}} : failed("Could not permanently delete this item.");
    }
    if (action == Action::Trash) {
        const auto folder = m_drive.sectionPath(StoreSection::Deleted);
        if (!confined(m_drive, folder, true) || !QFileInfo(folder).isDir()) return failed("The Deleted folder is unavailable.");
        if (source.startsWith(folder + '/')) return failed("This item is already in Deleted.");
        const auto destination = uniquePath(folder, sourceInfo.fileName(), false);
        if (destination.isEmpty() || !editable(destination)) return failed("The Deleted destination is unavailable.");
        if (moveWithoutCopy(source, destination)) return {destination, {}};
#ifdef Q_OS_MACOS
        // Native Files and private Deleted have separate volumes. Keep a local
        // recovery name until the verified private copy has been published.
        if (errno == EXDEV && DiskImage::filesRoot(m_drive.rootPath().toStdString())) {
            const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto staged = folder + "/.society-trash-" + id;
            const auto recovery = sourceInfo.absolutePath() + "/.society-trash-" + id;
            const auto cleanup = qScopeGuard([&] { removeTree(staged); });
            QString error;
            if (!copyTree(source, staged, &error)) return failed(error);
            if (!moveWithoutCopy(source, recovery)) return failed("The source changed before it could be moved to Deleted.");
            if (!moveWithoutCopy(staged, destination)) {
                moveWithoutCopy(recovery, source);
                return failed("Could not publish the item in Deleted. Its original remains in the source volume.");
            }
            removeTree(recovery);
            return {destination, {}};
        }
#endif
        return failed("Could not move this item to Deleted. Check access and that both folders are on the same filesystem.");
    }
    StorageMap map(m_drive);
    const auto sourceKey = StorageMap::logicalPath(m_drive.relativePath(source));
    const auto keys = map.files(sourceKey);
    // Never move a partial model package and leave its remote children behind.
    if (!keys.isEmpty() && !map.available(keys)) return failed("The selected item is not fully downloaded yet.");
    const auto syncDirectory = m_drive.rootPath() + "/.society-sync";
    if (QFileInfo(syncDirectory).isSymLink() || !QDir().mkpath(syncDirectory)) return failed("The Society operation directory is unavailable.");
    QLockFile lock(syncDirectory + "/operation.lock"); lock.setStaleLockTime(0);
    const bool copy = action == Action::Duplicate || action == Action::Copy;
    QString destination;
    const auto parent = QFileInfo(source).absolutePath();
    if (action == Action::Rename) {
        if (argument.isEmpty() || argument.startsWith('.') || argument.contains('/') || argument.contains('\\')
            || argument.contains(':') || argument.contains(QChar::Null)) return failed("Enter a valid file name.");
        destination = QDir(parent).filePath(argument);
        if (destination == source) return {source, {}};
    } else {
        const auto folder = action == Action::Copy ? argument : parent;
        if (!confined(m_drive, folder, true) || !QFileInfo(folder).isDir()) return failed("Choose a folder in this Society container.");
        if (folder == source || folder.startsWith(source + '/')) return failed("An item cannot be copied into itself.");
        destination = uniquePath(folder, QFileInfo(source).fileName(), action == Action::Duplicate);
    }
    if (destination.isEmpty() || !editable(destination)) return failed("The destination is unavailable or protected.");
    QString staged;
    auto cleanup = qScopeGuard([&] { if (!staged.isEmpty()) removeTree(staged); });
    if (copy) {
        staged = QFileInfo(destination).absolutePath() + "/.society-copy-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QString error; if (!copyTree(source, staged, &error)) return failed(error);
    }
    if (!lock.tryLock(2000)) return failed("Another Society operation is in progress. Try again.");
    if ((importing ? QFileInfo(source).canonicalFilePath() != source : !editable(source)) || !QFileInfo::exists(source)) return failed("The selected item changed. Try again.");
    if (!editable(destination) || QFileInfo::exists(destination) || QFileInfo(destination).isSymLink()) return failed("An item with that name already exists.");
    // QDir::rename works for both files and directories and never overwrites.
    if (!QDir().rename(copy ? staged : source, destination)) return failed("Could not move this item. Check access and try again.");
    staged.clear(); return {destination, {}};
}
}
