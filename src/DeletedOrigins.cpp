#include "DeletedOrigins.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <sys/stat.h>
#endif

namespace iiSocietyContainer::detail {
namespace {
void fail(QString *error, const QString &message) { if (error) *error = message; }
QString recoveryIdentifier(const SocietyDrive &drive) {
    return drive.storageKind() == SocietyDrive::StorageKind::LogicalDisk ? drive.storageIdentifier() : drive.identifier();
}
QString identity(const QString &path) {
#ifdef Q_OS_WIN
    const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(handle, &info);
    CloseHandle(handle);
    if (!ok || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    return QStringLiteral("%1:%2:%3").arg(info.dwVolumeSerialNumber).arg(info.nFileIndexHigh).arg(info.nFileIndexLow);
#else
    struct stat info{};
    if (::lstat(QFile::encodeName(path).constData(), &info) != 0 || S_ISLNK(info.st_mode)) return {};
    return QStringLiteral("%1:%2").arg(qulonglong(info.st_dev)).arg(qulonglong(info.st_ino));
#endif
}
}

DeletedOrigins::DeletedOrigins(const SocietyDrive &drive)
    : m_drive(drive), m_directory(drive.rootPath() + "/.society-deleted") {}

bool DeletedOrigins::lockPaths(const QStringList &deleted, QString *error, int timeoutMs) {
    unlock();
    if (error) error->clear();
    const QFileInfo before(m_directory);
    if (before.isSymLink() || before.isJunction() || (before.exists() && !before.isDir())
        || (!before.exists() && !QDir().mkdir(m_directory) && !QFileInfo(m_directory).isDir())) {
        fail(error, "The Deleted recovery journal is unavailable."); return false;
    }
    const QFileInfo after(m_directory);
    if (after.isSymLink() || after.isJunction() || after.canonicalFilePath() != m_directory) {
        fail(error, "The Deleted recovery journal is redirected."); return false;
    }
    QStringList keys;
    for (const auto &path : deleted) {
        const auto key = lockKey(path);
        if (key.isEmpty()) { fail(error, "The Deleted item is outside this drive."); return false; }
        if (!keys.contains(key)) keys.append(key);
    }
    if (keys.isEmpty()) { fail(error, "Choose an item in Deleted."); return false; }
    keys.sort(); // Acquiring multiple assets in a stable order avoids deadlock.
    for (const auto &key : keys) {
        const auto name = m_directory + "/item-" + QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".lock";
        if (QFileInfo(name).isSymLink() || QFileInfo(name).isJunction()) {
            unlock(); fail(error, "The Deleted item lock is redirected."); return false;
        }
        auto lease = std::make_unique<QLockFile>(name);
        lease->setStaleLockTime(0);
        if (!lease->tryLock(qMax(0, timeoutMs))) {
            unlock(); fail(error, "Another operation on this Deleted item is in progress."); return false;
        }
        m_keys.insert(key);
        m_locks.push_back(std::move(lease));
    }
    return true;
}

void DeletedOrigins::unlock() { m_locks.clear(); m_keys.clear(); }

QString DeletedOrigins::lockKey(const QString &deleted) const {
    const auto relative = m_drive.relativePath(deleted);
    if (!relative.startsWith("Deleted/")) return {};
    auto name = relative.section('/', 1, 1);
    if (name.isEmpty()) return {};
    const auto originalName = name;
    // Model descriptors and previews belong to their primary asset. A
    // top-level directory and every descendant use the same key.
    for (const auto *suffix : {".model.json", ".civitai.info", ".preview.png", ".preview.jpg", ".preview.webp"}) {
        const auto end = QLatin1String(suffix);
        if (name.endsWith(end, Qt::CaseInsensitive)) { name.chop(end.size()); break; }
    }
    for (const auto *suffix : {".safetensors", ".safetensor", ".gguf", ".ggml", ".ckpt", ".bin", ".pt", ".pth", ".onnx", ".tflite", ".pb", ".h5", ".hdf5", ".npz", ".npy", ".model", ".mlmodel", ".engine"}) {
        const auto end = QLatin1String(suffix);
        if (name.endsWith(end, Qt::CaseInsensitive)) { name.chop(end.size()); break; }
    }
    if (name.isEmpty()) name = originalName;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    name = name.normalized(QString::NormalizationForm_D).toCaseFolded();
#endif
    return name;
}

bool DeletedOrigins::owns(const QString &deleted) const { return m_keys.contains(lockKey(deleted)); }

bool DeletedOrigins::reserved(const QString &deleted) const {
    const auto path = recordPath(deleted);
    const QFileInfo info(path);
    return !path.isEmpty() && (info.exists() || info.isSymLink() || info.isJunction());
}

QString DeletedOrigins::recordPath(const QString &deleted) const {
    const auto relative = m_drive.relativePath(deleted);
    if (!relative.startsWith("Deleted/")) return {};
    return m_directory + '/' + QString::fromLatin1(QCryptographicHash::hash(relative.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".json";
}

bool DeletedOrigins::write(const QString &deleted, const QString &original, bool committed, QString *error) {
    const auto target = recordPath(deleted);
    const auto relative = m_drive.relativePath(original);
    const auto id = committed ? identity(deleted) : QString();
    if (!owns(deleted) || target.isEmpty() || relative.isEmpty() || relative.startsWith("Deleted/")
        || m_drive.resolvePath(relative) != original || (committed && id.isEmpty())
        || QFileInfo(target).isSymLink() || QFileInfo(target).isJunction()) {
        fail(error, "The Deleted recovery location could not be recorded safely."); return false;
    }
    const QJsonObject data{{"schema", 1}, {"container", recoveryIdentifier(m_drive)},
        {"deleted", m_drive.relativePath(deleted)}, {"original", relative},
        {"committed", committed}, {"identity", id}};
    QSaveFile file(target);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(bytes) != bytes.size() || !file.commit()) {
        fail(error, "Could not save the Deleted recovery location."); return false;
    }
    return true;
}

DeletedOrigins::Lookup DeletedOrigins::find(const QString &deleted) const {
    const auto root = m_drive.sectionPath(StoreSection::Deleted);
    if (!owns(deleted) || !deleted.startsWith(root + '/')) return {{}, {}, "Only items in Deleted can be restored."};
    auto item = deleted;
    while (item.startsWith(root + '/')) {
        const auto path = recordPath(item);
        const QFileInfo info(path);
        if (info.exists() || info.isSymLink()) {
            if (!info.isFile() || info.isSymLink() || info.isJunction() || info.size() > 16384)
                return {{}, {}, "The Deleted recovery record is invalid."};
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return {{}, {}, "The Deleted recovery record cannot be read."};
            const auto object = QJsonDocument::fromJson(file.readAll()).object();
            const auto relative = object.value("original").toString();
            const auto original = m_drive.resolvePath(relative);
            const auto expectedId = object.value("identity").toString();
            if (object.value("schema") != 1 || object.value("container").toString() != recoveryIdentifier(m_drive)
                || object.value("deleted").toString() != m_drive.relativePath(item)
                || !object.value("committed").toBool() || expectedId.isEmpty() || identity(item) != expectedId
                || original.isEmpty() || relative.startsWith("Deleted/"))
                return {{}, {}, "The Deleted recovery record no longer matches this item."};
            return {original + deleted.mid(item.size()), item, {}};
        }
        item = QFileInfo(item).absolutePath();
    }
    return {};
}

void DeletedOrigins::forget(const QString &deleted) const {
    const auto path = recordPath(deleted);
    if (owns(deleted) && !path.isEmpty() && !QFileInfo(path).isSymLink() && !QFileInfo(path).isJunction()) QFile::remove(path);
}
}
