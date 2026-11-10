#include <iiFileProviderStorage.h>
#include <iiFileProviderDevice.h>
#include <iiFileProviderPresentation.h>
#include <ObjectSource.h>
#include <FileCatalog.h>
#include "FileOperations.h"
#include "StorageMap.h"
#include "SharedStorage.h"
#include "ModelStore.h"
#include "DiskImage.h"
#include "src/AssetCompanions.h"
#include "src/AssetTransfer.h"
#include "src/DeletedOrigins.h"
#include "src/ExclusiveRename.h"
#include "src/FileTreePaths.h"
#include <QFileInfo>
#include <QScopeGuard>
#include <QUuid>
#include <cerrno>
#include <mutex>
#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif
#ifdef Q_OS_MACOS
#include <copyfile.h>
#endif

namespace iiSocietyContainer {
namespace {
std::mutex relocationMutex;
FileOperations::RelocationHandler relocationHandler;
class RelocationNotice final {
public:
    RelocationNotice(QString container, QString source, QString destination)
        : m_container(std::move(container)), m_source(std::move(source)), m_destination(std::move(destination)) {
        { std::lock_guard lock(relocationMutex); m_handler = relocationHandler; }
        if (m_handler) m_handler(m_container, m_source, m_destination, FileOperations::RelocationPhase::Prepare);
    }
    ~RelocationNotice() {
        if (m_handler && m_pending) {
            try { m_handler(m_container, m_source, m_destination, FileOperations::RelocationPhase::Cancel); }
            catch (...) {}
        }
    }
    void commit() {
        m_pending = false; // A failed commit must retain the prepared recovery record.
        if (m_handler) m_handler(m_container, m_source, m_destination, FileOperations::RelocationPhase::Commit);
    }
    void retain() { m_pending = false; }
private:
    QString m_container, m_source, m_destination;
    FileOperations::RelocationHandler m_handler;
    bool m_pending = true;
};
bool modelPath(const SocietyDrive &drive, const QString &path) {
    return drive.relativePath(path).section('/', 0, 0) == storeSectionName(StoreSection::Models);
}
bool confined(const SocietyDrive &drive, const QString &path, bool section = false) {
    if (!drive.isValid() || !QFileInfo(path).isAbsolute() || path != iiFileProvider::Directory::cleanPath(path)) return false;
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
            || part.contains(QChar::Null) || part.startsWith(".society-") || part.startsWith(".iiwebserver-")
            || iiFileProvider::native::Presentation::isControlFile(detail::nativePath(part))) return false;
        if (first) { first = false; continue; }
        current += '/' + part; const QFileInfo info(current);
        if (info.isSymLink() || info.isJunction() || (info.exists() && info.canonicalFilePath() != current)) return false;
    }
    return true;
}
bool caseOnlyAlias(const QFileInfo &source, const QString &name) {
    if (name == source.fileName() || name.compare(source.fileName(), Qt::CaseInsensitive) != 0
        || name.contains('/') || name.contains('\\') || name.contains(':') || name.contains(QChar::Null)) return false;
    const auto parent = source.absoluteDir();
    const auto target = parent.filePath(name);
    const QFileInfo other(target);
    if (!other.exists() || other.isSymLink() || other.isJunction()) return false;
    // Distinct hard links are distinct directory entries, even when they have
    // the same inode. Only use a temporary name for a spelling of this entry.
    const auto entries = parent.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    if (entries.contains(name, Qt::CaseSensitive)) return false;
    std::error_code error;
    return std::filesystem::equivalent(detail::nativePath(source.absoluteFilePath()), detail::nativePath(target), error) && !error;
}
bool copyTree(const QString &source, const QString &destination, QString *error) {
    const QFileInfo before(source);
    if (before.isSymLink() || before.isJunction() || (!before.isFile() && !before.isDir())) {
        *error = "This item contains an unavailable file or a symbolic link."; return false;
    }
    if (before.isDir()) {
        if (!iiFileProvider::Directory().mkdir(destination)) { *error = "Could not create the copy."; return false; }
        const iiFileProvider::Directory directory(source);
        const auto entries = directory.entryList(iiFileProvider::Directory::AllEntries | iiFileProvider::Directory::Hidden | iiFileProvider::Directory::System | iiFileProvider::Directory::NoDotAndDotDot, iiFileProvider::Directory::Name);
        if (!before.isReadable()) { *error = "The source folder cannot be read."; return false; }
        for (const auto &name : entries) {
            if (name == ".iisacc-temporary.lock") continue;
            if (!copyTree(directory.filePath(name), iiFileProvider::Directory(destination).filePath(name), error)) return false;
        }
        if (entries != directory.entryList(iiFileProvider::Directory::AllEntries | iiFileProvider::Directory::Hidden | iiFileProvider::Directory::System | iiFileProvider::Directory::NoDotAndDotDot, iiFileProvider::Directory::Name)) {
            *error = "The folder changed while it was being copied. Try again."; return false;
        }
    } else {
        bool copied = false;
#ifdef Q_OS_MACOS
        // APFS clones avoid rereading multi-GB model weights. Other filesystems
        // transparently fall back to copyfile's ordinary data copy.
        copied = ::copyfile(iiFileProvider::FileDevice::encodeName(source).constData(), iiFileProvider::FileDevice::encodeName(destination).constData(),
                            nullptr, COPYFILE_ALL | COPYFILE_CLONE | COPYFILE_EXCL | COPYFILE_NOFOLLOW_SRC) == 0;
#else
        copied = iiFileProvider::FileDevice::copy(source, destination);
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
#ifdef Q_OS_WIN
    const QFileInfo info(path);
    if (info.isDir()) {
        if (info.isSymLink() || info.isJunction())
            return RemoveDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16())) != FALSE;
        const auto children = iiFileProvider::Directory(path).entryInfoList(iiFileProvider::Directory::AllEntries | iiFileProvider::Directory::Hidden | iiFileProvider::Directory::System | iiFileProvider::Directory::NoDotAndDotDot);
        for (const auto &child : children)
            if (!removeTree(child.absoluteFilePath())) return false;
        return iiFileProvider::Directory().rmdir(path);
    }
    return iiFileProvider::FileDevice::remove(path);
#else
    return QFileInfo(path).isDir() && !QFileInfo(path).isSymLink() ? iiFileProvider::Directory(path).removeRecursively() : iiFileProvider::FileDevice::remove(path);
#endif
}
bool moveWithoutCopy(const QString &source, const QString &destination) {
    return detail::renameWithoutCopy(source, destination);
}
bool assetMoves(const QString &source, const QString &destination, bool model,
                QList<QPair<QString, QString>> *moves, QString *error, bool stabilize = false,
                QStringList *reserved = nullptr) {
    const auto object = detail::AssetObject::plan(source, destination, model, stabilize, error);
    if (!object) return false;
    *moves = object->members;
    if (reserved) *reserved = object->reservedTargets;
    return true;
}
QString uniqueAssetPath(const QString &folder, const QString &name, const QString &source,
                        bool duplicate, bool model, QString *error,
                        const std::function<bool(const QString &)> &catalogReserved = {}) {
    const QFileInfo info(name); const auto suffix = info.suffix();
    const auto ext = suffix.isEmpty() ? QString() : '.' + suffix;
    const auto stem = ext.isEmpty() ? name : name.chopped(ext.size());
    for (int n = 1; n <= 10000; ++n) {
        const auto candidate = iiFileProvider::Directory(folder).filePath(
            stem + (duplicate ? (n == 1 ? QStringLiteral(" copy") : QStringLiteral(" copy %1").arg(n))
                              : (n == 1 ? QString() : QStringLiteral(" %1").arg(n))) + ext);
        QList<QPair<QString, QString>> moves; QStringList targets;
        if (!assetMoves(source, candidate, model, &moves, error, false, &targets)) return {};
        bool available = true;
        for (const auto &path : targets) {
            const QFileInfo target(path);
            if (target.exists() || target.isSymLink() || target.isJunction()) { available = false; break; }
        }
        if (available && catalogReserved) {
            try {
                for (const auto &move : moves) {
                    if (!iiFileProvider::native::Presentation::isControlFile(detail::nativePath(move.second).filename())
                        && catalogReserved(move.second)) { available = false; break; }
                }
            } catch (const std::exception &failure) {
                if (error) *error = QString::fromUtf8(failure.what());
                return {};
            }
        }
        if (available) return candidate;
    }
    if (error) *error = QStringLiteral("No unused name is available for the asset and its companions.");
    return {};
}
bool moveAssets(const QList<QPair<QString, QString>> &moves, QString *error, bool *restored, int *nativeError = nullptr) {
    if (restored) *restored = true;
    for (const auto &move : moves) {
        const QFileInfo target(move.second);
        if (target.exists() || target.isSymLink() || target.isJunction()) {
            if (error) *error = QStringLiteral("An asset or its metadata already exists at the destination.");
            if (nativeError) *nativeError = EEXIST;
            return false;
        }
    }
    QList<QPair<QString, QString>> completed;
    for (const auto &move : moves) {
        if (moveWithoutCopy(move.first, move.second)) { completed.append(move); continue; }
        const int code = errno;
        bool rolledBack = true;
        for (auto it = completed.crbegin(); it != completed.crend(); ++it)
            rolledBack = moveWithoutCopy(it->second, it->first) && rolledBack;
        if (restored) *restored = rolledBack;
        if (nativeError) *nativeError = code;
        if (error) *error = rolledBack
            ? QStringLiteral("Could not move the asset and all of its metadata. Existing files were not replaced.")
            : QStringLiteral("The asset move could not be fully rolled back. Check both locations before retrying.");
        return false;
    }
    return true;
}
}
void FileOperations::setRelocationHandler(RelocationHandler handler) {
    std::lock_guard lock(relocationMutex);
    relocationHandler = std::move(handler);
}
FileOperations::FileOperations(SocietyDrive drive) : m_drive(std::move(drive)) {}
std::optional<SocietyDrive> FileOperations::containingDrive(const QString &path) {
    if (!QFileInfo(path).isAbsolute()) return {};
    if (const auto root = DiskImage::containerRoot(path.toStdString())) return SocietyDrive::open(QString::fromStdString(root->string()));
    QString parent = iiFileProvider::Directory::cleanPath(path);
    for (;;) {
        if (QFileInfo::exists(iiFileProvider::Directory(parent).filePath(".society-drive.json"))) return SocietyDrive::open(parent);
        const auto ancestor = QFileInfo(parent).absolutePath();
        if (ancestor == parent) break; // Every Windows volume has its own root.
        parent = ancestor;
    }
    return {};
}
bool FileOperations::editable(const QString &path) const {
    return confined(m_drive, path);
}
FileOperations::Result FileOperations::createDirectory(const QString &parent, const QString &name) const {
    auto failed = [](const QString &error) { return Result{{}, error}; };
    if (!m_drive.isReady() || !confined(m_drive, parent, true) || !QFileInfo(parent).isDir())
        return failed("Choose an available folder in this Society container.");
    if (name.isEmpty() || name.startsWith('.') || name.contains('/') || name.contains('\\')
        || name.contains(':') || name.contains(QChar::Null)) return failed("Enter a valid folder name.");
    const auto destination = iiFileProvider::Directory(parent).filePath(name);
    if (!editable(destination)) return failed("The destination is unavailable or protected.");
    const auto syncDirectory = m_drive.rootPath() + "/.society-sync";
    if (QFileInfo(syncDirectory).isSymLink() || !iiFileProvider::Directory().mkpath(syncDirectory))
        return failed("The Society operation directory is unavailable.");
    iiFileProvider::LockFile lock(syncDirectory + "/operation.lock"); lock.setStaleLockTime(0);
    if (!lock.tryLock(2000)) return failed("Another Society operation is in progress. Try again.");
    if (!m_drive.isReady() || !confined(m_drive, parent, true) || !QFileInfo(parent).isDir() || !editable(destination))
        return failed("The selected folder changed. Try again.");
    std::error_code error;
    if (!iiFileProvider::native::createDirectory(detail::nativePath(destination), error))
        return failed(error ? QString::fromStdString(error.message()) : QStringLiteral("An item with that name already exists."));
    return {destination, {}};
}
FileOperations::Result FileOperations::perform(Action action, const QString &source, const QString &argument) const {
    auto failed = [](const QString &error) { return Result{{}, error}; };
    try {
    if ((action == Action::Move || action == Action::Restore || action == Action::RestoreTo) && !m_drive.isReady()) return failed("The Society drive is not ready.");
    const bool importing = action == Action::Copy;
    const QFileInfo sourceInfo(source);
    if (importing ? (!sourceInfo.isAbsolute() || sourceInfo.canonicalFilePath() != source || sourceInfo.isSymLink() || sourceInfo.isJunction()
                     || iiFileProvider::native::Presentation::isControlFile(detail::nativePath(sourceInfo.fileName()))) : !editable(source))
        return failed("This protected folder or redirected path cannot be changed.");
    const bool deleting = action == Action::Trash || action == Action::Remove;
    const bool restoring = action == Action::Restore || action == Action::RestoreTo;
    const bool direct = deleting || restoring;
    if (!QFileInfo::exists(source)) return failed(deleting
        ? "This item is not stored on this device. Delete it on the Society host."
        : restoring ? "This item is not stored on this device. Put it back on the Society host."
        : "Download this item from the Society host before changing it.");
    QString storageError;
    const auto storage = SharedStorage::open(m_drive.rootPath(), &storageError);
    if (!storage) return failed(storageError);
    const auto sourceSection = m_drive.sectionForPath(source);
    // A project package owns its own revisions and must move as one directory;
    // its internal files are deliberately excluded from managed file capture.
    const bool projectPackage = sourceInfo.isDir()
        && sourceInfo.suffix().compare(QStringLiteral("iiproject"), Qt::CaseInsensitive) == 0;
    if (projectPackage && (action == Action::Duplicate || action == Action::Copy))
        return failed("Project package copying requires a project-aware copy operation.");
    if (projectPackage && action == Action::Rename
        && QFileInfo(argument).suffix().compare(QStringLiteral("iiproject"), Qt::CaseInsensitive) != 0)
        return failed("A project package must keep its .iiproject extension.");
    const bool directModelCopy = sourceInfo.isFile() &&
        ((action == Action::Duplicate && sourceSection == StoreSection::Models) ||
         (action == Action::Copy && m_drive.sectionForPath(argument) == StoreSection::Models));
    const bool modelRename = action == Action::Rename && sourceSection == StoreSection::Models
        && (sourceInfo.isFile() || (sourceInfo.isDir() && ModelClassifier::isPackage(source)));
    if (action == Action::Rename && !modelRename && caseOnlyAlias(sourceInfo, argument)) {
        const auto suffix = sourceInfo.suffix();
        const auto temporaryName = QStringLiteral("SocietyRename-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))
            + (suffix.isEmpty() ? QString() : '.' + suffix);
        const auto first = perform(Action::Rename, source, temporaryName);
        if (!first) return first;
        const auto second = perform(Action::Rename, first.path, argument);
        if (second) return second;
        return {first.path, QStringLiteral("The item is temporarily named %1 because the case change could not finish: %2")
            .arg(first.path, second.error)};
    }
    if (modelRename) {
        if (!StorageMap(m_drive).isLocalAuthority())
            return failed("Rename this model on its Society host.");
        QString error;
        const auto store = ModelStore::open(m_drive.rootPath(), &error);
        if (!store) return failed(error);
        const auto models = m_drive.sectionPath(StoreSection::Models);
        auto repository = storage->fileRepository();
        QString destination;
        QString partialError;
        try {
            // Serialize only the rename and its metadata updates with the
            // indexer. Never snapshot or hash model weights for a path change.
            repository->withExclusiveOperation([&] {
                const auto moveAndTrack = [&](const QString &from, const QString &name) {
                    QList<QPair<QString, QString>> moved;
                    const auto intended = QFileInfo(from).absoluteDir().filePath(name);
                    std::unique_ptr<RelocationNotice> notice;
                    if (intended != from) notice = std::make_unique<RelocationNotice>(m_drive.rootPath(), from, intended);
                    const auto next = store->rename(from, name, &error, &moved);
                    if (next.isEmpty()) return false;
                    destination = next;
                    if (next == from) return true;
                    if (next != intended) { notice->retain(); throw std::runtime_error("Model destination changed during semantic preparation"); }
                    notice->commit();
                    for (const auto &asset : moved) {
                        const auto oldKey = storage->managedFilePath(StoreSection::Models,
                            iiFileProvider::Directory(models).relativeFilePath(asset.first));
                        const auto newKey = storage->managedFilePath(StoreSection::Models,
                            iiFileProvider::Directory(models).relativeFilePath(asset.second));
                        if (QFileInfo(asset.second).isDir() || repository->lookup(oldKey))
                            repository->relocated(oldKey, newKey, false);
                        if (QFileInfo(asset.second).isFile()) {
                            QString indexError;
                            if (store->indexAsset(asset.second, &indexError).isEmpty()) {
                                partialError = QStringLiteral("The model was renamed to %1, but its catalog needs synchronization: %2")
                                    .arg(destination, indexError);
                                return false;
                            }
                        }
                    }
                    return true;
                };
                if (!caseOnlyAlias(sourceInfo, argument)) { (void)moveAndTrack(source, argument); return; }
                const auto suffix = sourceInfo.suffix();
                const auto temporaryName = QStringLiteral("SocietyRename-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))
                    + (suffix.isEmpty() ? QString() : '.' + suffix);
                if (!moveAndTrack(source, temporaryName)) return;
                const auto temporary = destination;
                if (!moveAndTrack(temporary, argument)) {
                    const auto reason = error.isEmpty() ? QStringLiteral("The requested name is unavailable.") : error;
                    partialError = QStringLiteral("The model is temporarily named %1 because the case change could not finish: %2. Rename it again to continue.")
                        .arg(temporary, reason);
                }
            });
        } catch (const std::exception &e) {
            const auto reason = QString::fromUtf8(e.what());
            if (!destination.isEmpty() && destination != source)
                return {destination, QStringLiteral("The model was renamed to %1, but its managed index needs synchronization: %2")
                    .arg(destination, reason)};
            return failed(reason);
        }
        if (!partialError.isEmpty()) return {destination, partialError};
        return destination.isEmpty()
            ? failed(error.isEmpty() ? QStringLiteral("Could not rename this model. Try again.") : error)
            : Result{destination, {}};
    }
    std::unique_ptr<FileRepository> repository;
    const auto repositoryId = m_drive.storageKind() == SocietyDrive::StorageKind::LogicalDisk ? m_drive.storageIdentifier() : m_drive.identifier();
    const bool repositoryAvailable = !directModelCopy && (!direct || std::filesystem::exists(
        FileRepository::packagePath(detail::nativePath(m_drive.rootPath()), repositoryId.toStdString()) / "objects.sqlite3"));
    const auto openRepository = [&] {
        if (!repository && repositoryAvailable) {
            FileRepositoryContext context;
            context.description = deleting ? "Delete Society file" : importing ? "Import Society file" : "Move or copy Society file";
            repository = storage->fileRepository(std::move(context));
        }
    };
    if (!direct) openRepository();
    const auto logicalSource = !importing && sourceSection ? storage->managedFilePath(*sourceSection,
        iiFileProvider::Directory(m_drive.sectionPath(*sourceSection)).relativeFilePath(source)) : std::string{};
    if (!importing && !direct && repository && !projectPackage) repository->captureTree(logicalSource);
    const auto refreshModelCatalog = [&](const QString &destination, const QList<QPair<QString, QString>> &group) {
        if (m_drive.sectionForPath(destination) != StoreSection::Models) return;
        QString catalogError;
        const auto models = ModelStore::open(m_drive.rootPath(), &catalogError);
        if (!models) throw std::runtime_error(catalogError.toStdString());
        const auto index = [&](const QString &path) {
            if (!QFileInfo(path).isFile() || iiFileProvider::native::Presentation::isControlFile(detail::nativePath(path).filename())) return;
            if (models->indexAsset(path, &catalogError).isEmpty()) throw std::runtime_error(catalogError.toStdString());
        };
        index(destination);
        for (qsizetype i = 1; i < group.size(); ++i) index(group[i].second);
    };
    std::unique_ptr<RelocationNotice> relocation;
    const auto complete = [&](const QString &destination, bool moved,
                              const QList<QPair<QString, QString>> &group = {}) -> Result {
        try {
        if (moved && relocation) relocation->commit();
        openRepository();
        if (!repository) {
            if (moved || directModelCopy) refreshModelCatalog(destination, group);
            return {destination, {}};
        }
        const auto section = m_drive.sectionForPath(destination); if (!section) return failed("The destination left this drive.");
        const auto logical = storage->managedFilePath(*section, iiFileProvider::Directory(m_drive.sectionPath(*section)).relativeFilePath(destination));
        if (moved) {
            repository->relocated(logicalSource, logical, !direct);
            for (qsizetype i = 1; i < group.size(); ++i) {
                const auto &asset = group[i];
                if (iiFileProvider::native::Presentation::isControlFile(detail::nativePath(asset.first).filename())) continue;
                const auto fromSection = m_drive.sectionForPath(asset.first), toSection = m_drive.sectionForPath(asset.second);
                if (!fromSection || !toSection) continue;
                const auto from = storage->managedFilePath(*fromSection,
                    iiFileProvider::Directory(m_drive.sectionPath(*fromSection)).relativeFilePath(asset.first));
                const auto to = storage->managedFilePath(*toSection,
                    iiFileProvider::Directory(m_drive.sectionPath(*toSection)).relativeFilePath(asset.second));
                if (repository->lookup(from)) repository->relocated(from, to, false);
            }
            refreshModelCatalog(destination, group);
        }
        else if (!importing && sourceInfo.isFile()) {
            const auto original = repository->lookup(logicalSource);
            repository->observe(logical, original ? std::optional<iiFileProvider::FileMetadata>(repository->original(original->key).metadata.annotations) : std::nullopt);
        } else repository->captureTree(logical);
        if (!moved) {
            for (qsizetype i = 1; i < group.size(); ++i) {
                const auto &asset = group[i];
                if (iiFileProvider::native::Presentation::isControlFile(detail::nativePath(asset.first).filename())) continue;
                const auto toSection = m_drive.sectionForPath(asset.second);
                if (!toSection) continue;
                const auto to = storage->managedFilePath(*toSection,
                    iiFileProvider::Directory(m_drive.sectionPath(*toSection)).relativeFilePath(asset.second));
                std::optional<iiFileProvider::FileMetadata> annotations;
                if (!importing) {
                    if (const auto fromSection = m_drive.sectionForPath(asset.first)) {
                        const auto from = storage->managedFilePath(*fromSection,
                            iiFileProvider::Directory(m_drive.sectionPath(*fromSection)).relativeFilePath(asset.first));
                        if (const auto original = repository->lookup(from)) annotations = repository->original(original->key).metadata.annotations;
                    }
                }
                repository->observe(to, annotations);
            }
        }
        return {destination, {}};
        } catch (const std::exception &e) {
            return {destination, QStringLiteral("The asset is at %1, but its managed index needs synchronization: %2")
                .arg(destination, QString::fromUtf8(e.what()))};
        }
    };
    // Deletion has no dependency on catalog residency, downloads, hashes, model
    // packages, or the replication lock. The indexer observes changes afterwards.
    if (action == Action::Remove) {
        if (!source.startsWith(m_drive.sectionPath(StoreSection::Deleted) + '/')) return failed("Only items in Deleted can be permanently removed.");
        detail::DeletedOrigins origins(m_drive); QString originError;
        if (!origins.lockPaths({source}, &originError)) return failed(originError);
        const auto recorded = origins.find(source);
        if (!recorded.error.isEmpty()) return failed(recorded.error);
        const bool model = !recorded.original.isEmpty() && modelPath(m_drive, recorded.original);
        QList<QPair<QString, QString>> moves; QString moveError;
        if (!assetMoves(source, source + ".society-removal", model, &moves, &moveError, true)) return failed(moveError);
        if (!removeTree(source)) return failed("Could not permanently delete this item.");
        for (qsizetype i = 1; i < moves.size(); ++i) {
            const auto path = moves[i].first;
            if (QFileInfo::exists(path) && !removeTree(path))
                return {source, QStringLiteral("The asset was removed, but a companion remains at %1.").arg(path)};
        }
        origins.forget(source);
        if (repositoryAvailable) {
            try {
                openRepository();
                repository->removed(logicalSource);
                for (qsizetype i = 1; i < moves.size(); ++i) {
                    const auto path = moves[i].first;
                    if (iiFileProvider::native::Presentation::isControlFile(detail::nativePath(path).filename())) continue;
                    const auto key = storage->managedFilePath(StoreSection::Deleted,
                        iiFileProvider::Directory(m_drive.sectionPath(StoreSection::Deleted)).relativeFilePath(path));
                    if (repository->lookup(key)) repository->removed(key);
                }
            } catch (const std::exception &e) {
                return {source, QStringLiteral("The asset was removed, but its managed index needs synchronization: %1").arg(QString::fromUtf8(e.what()))};
            }
        }
        return {source, {}};
    }
    if (action == Action::Trash) {
        const auto folder = m_drive.sectionPath(StoreSection::Deleted);
        if (!confined(m_drive, folder, true) || !QFileInfo(folder).isDir()) return failed("The Deleted folder is unavailable.");
        if (source.startsWith(folder + '/')) return failed("This item is already in Deleted.");
        detail::DeletedOrigins origins(m_drive); QString originError;
        QString moveError, destination;
        // The journal reservation participates in name selection. Recheck it
        // after acquiring the item's lease so concurrent same-name requests
        // cannot publish or overwrite one another's records.
        for (int attempt = 0; attempt < 10000; ++attempt) {
            const auto choose = [&] {
                return uniqueAssetPath(folder, sourceInfo.fileName(), source, false,
                    sourceSection == StoreSection::Models, &moveError,
                    [&](const QString &path) { return origins.reserved(path); });
            };
            destination = choose();
            if (destination.isEmpty()) break;
            // Also reserve the source's unnumbered name: two requests for the
            // same source must not start separate cross-volume copies.
            const auto sourceReservation = iiFileProvider::Directory(folder).filePath(sourceInfo.fileName());
            if (!origins.lockPaths({sourceReservation, destination}, &originError)) return failed(originError);
            if (choose() == destination) break;
            origins.unlock(); destination.clear();
        }
        if (destination.isEmpty() || !editable(destination))
            return failed(moveError.isEmpty() ? QStringLiteral("The Deleted destination is unavailable.") : moveError);
        QList<QPair<QString, QString>> moves;
        if (!assetMoves(source, destination, sourceSection == StoreSection::Models, &moves, &moveError, true)) return failed(moveError);
        relocation = std::make_unique<RelocationNotice>(m_drive.rootPath(), source, destination);
        if (!origins.write(destination, source, false, &originError)) return failed(originError);
        bool restored = true; int nativeError = 0;
        if (moveAssets(moves, &moveError, &restored, &nativeError)) {
            if (!origins.write(destination, source, true, &originError)) {
                QList<QPair<QString, QString>> reverse;
                for (auto it = moves.crbegin(); it != moves.crend(); ++it) reverse.append({it->second, it->first});
                if (moveAssets(reverse, nullptr, nullptr)) { origins.forget(destination); return failed(originError); }
                relocation->retain();
                return {destination, originError + " The item remains in Deleted; copy it manually to recover it."};
            }
            return complete(destination, true, moves);
        }
        if (!restored) { relocation->retain(); return {QFileInfo::exists(destination) ? destination : source, moveError}; }
#ifdef Q_OS_MACOS
        // Native Files and private Deleted have separate volumes. Keep a local
        // recovery name until the verified private copy has been published.
        if (nativeError == EXDEV && DiskImage::filesRoot(m_drive.rootPath().toStdString())) {
            const auto transferred = detail::transferAssetGroup(moves, copyTree, moveAssets, removeTree,
                [&](QString *error) { return origins.write(destination, source, true, error); });
            if (!transferred.committed) {
                if (transferred.originalsRestored && !QFileInfo::exists(destination)) origins.forget(destination);
                if (!transferred.originalsRestored || QFileInfo::exists(destination)) relocation->retain();
                return {QFileInfo::exists(destination) ? destination : (transferred.originalsRestored ? QString() : source), transferred.error};
            }
            auto result = complete(destination, true, moves);
            if (!transferred.error.isEmpty())
                result.error = result.error.isEmpty() ? transferred.error : result.error + ' ' + transferred.error;
            return result;
        }
#endif
        origins.forget(destination);
        return failed(moveError.isEmpty() ? QStringLiteral("Could not move this item to Deleted.") : moveError);
    }
    if (restoring) {
        const auto deleted = m_drive.sectionPath(StoreSection::Deleted);
        if (!source.startsWith(deleted + '/')) return failed("Only items in Deleted can be restored.");
        StorageMap replica(m_drive);
        if (!replica.isLocalAuthority()) {
            const auto objects = replica.files(StorageMap::logicalPath(m_drive.relativePath(source)));
            if (!objects.isEmpty() && !replica.available(objects))
                return failed("This item is not fully available on this device. Put it back on the Society host.");
        }
        detail::DeletedOrigins origins(m_drive); QString originError;
        if (!origins.lockPaths({source}, &originError)) return failed(originError);
        const auto recorded = origins.find(source);
        if (action == Action::Restore && !recorded.error.isEmpty()) return failed(recorded.error);
        QString original;
        if (action == Action::RestoreTo) {
            if (!confined(m_drive, argument, true) || !QFileInfo(argument).isDir()
                || argument == deleted || argument.startsWith(deleted + '/'))
                return failed("Choose a folder in this Society container outside Deleted.");
            original = iiFileProvider::Directory(argument).filePath(sourceInfo.fileName());
        } else original = recorded.original;
        // Older managed files can recover their path from the native object
        // history. An untracked legacy item has no trustworthy origin.
        if (action == Action::Restore && original.isEmpty() && repositoryAvailable && sourceInfo.isFile()) {
            openRepository();
            if (const auto current = repository->lookup(logicalSource)) {
                // A stale index may describe a different file now occupying
                // this path. Verify its metadata stamp without reading payload.
                const auto identity = iiFileProvider::ObjectSource::inspect(detail::nativePath(source));
                if (current->sourceStamp.empty() || current->sourceStamp != identity.stamp)
                    return failed("The older item's original location cannot be verified. Copy it to the desired folder instead.");
                std::uint64_t after = 0;
                for (;;) {
                    const auto page = repository->objects().history(current->key, after, 256);
                    if (page.empty()) break;
                    for (const auto &version : page) {
                        after = version.version;
                        const auto relative = StorageMap::physicalPath(QString::fromStdString(version.path));
                        if (!relative.isEmpty() && !relative.startsWith("Deleted/"))
                            original = m_drive.resolvePath(relative);
                    }
                    if (page.size() < 256) break;
                }
            }
        }
        if (original.isEmpty()) return failed("The original location is unavailable. Choose a destination folder to put this item back.");
        const auto relative = m_drive.relativePath(original);
        if (relative.isEmpty() || relative.startsWith("Deleted/") || m_drive.resolvePath(relative) != original || !editable(original))
            return failed("The original location is unavailable or redirected.");
        const auto parent = QFileInfo(original).absolutePath();
        if (!confined(m_drive, parent, true) || (!QFileInfo(parent).isDir() && !iiFileProvider::Directory().mkpath(parent))
            || !confined(m_drive, parent, true)) return failed("Could not reopen the original folder safely.");
        const bool model = modelPath(m_drive, original);
        QString moveError;
        const auto destination = uniqueAssetPath(parent, QFileInfo(original).fileName(), source, false, model, &moveError);
        if (destination.isEmpty() || !editable(destination))
            return failed(moveError.isEmpty() ? QStringLiteral("The original destination is unavailable.") : moveError);
        QList<QPair<QString, QString>> moves;
        if (!assetMoves(source, destination, model, &moves, &moveError, true)) return failed(moveError);
        relocation = std::make_unique<RelocationNotice>(m_drive.rootPath(), source, destination);
        QString transferWarning;
        bool restored = true; int nativeError = 0;
        if (!moveAssets(moves, &moveError, &restored, &nativeError)) {
            if (!restored) { relocation->retain(); return {QFileInfo::exists(destination) ? destination : source, moveError}; }
#ifdef Q_OS_MACOS
            if (nativeError == EXDEV && DiskImage::filesRoot(m_drive.rootPath().toStdString())) {
                const auto transferred = detail::transferAssetGroup(moves, copyTree, moveAssets, removeTree,
                    [](QString *) { return true; });
                if (!transferred.committed) {
                    if (!transferred.originalsRestored || QFileInfo::exists(destination)) relocation->retain();
                    return {QFileInfo::exists(destination) ? destination : (transferred.originalsRestored ? QString() : source), transferred.error};
                }
                transferWarning = transferred.error;
            } else
#endif
                return failed(moveError.isEmpty() ? QStringLiteral("Could not restore this item without overwriting another file.") : moveError);
        }
        if (recorded.recordedItem == source || action == Action::RestoreTo) origins.forget(source);
        auto result = complete(destination, true, moves);
        if (!transferWarning.isEmpty())
            result.error = result.error.isEmpty() ? transferWarning : result.error + ' ' + transferWarning;
        return result;
    }
    StorageMap map(m_drive);
    const auto sourceKey = StorageMap::logicalPath(m_drive.relativePath(source));
    const auto keys = map.files(sourceKey);
    // Never move a partial model package and leave its remote children behind.
    if (!keys.isEmpty() && !map.available(keys)) return failed("The selected item is not fully downloaded yet.");
    const auto syncDirectory = m_drive.rootPath() + "/.society-sync";
    if (QFileInfo(syncDirectory).isSymLink() || !iiFileProvider::Directory().mkpath(syncDirectory)) return failed("The Society operation directory is unavailable.");
    iiFileProvider::LockFile lock(syncDirectory + "/operation.lock"); lock.setStaleLockTime(0);
    const bool copy = action == Action::Duplicate || action == Action::Copy;
    std::unique_ptr<iiFileProvider::FileCatalog> destinationCatalog;
    const auto catalogReserved = [&](const QString &path) {
        if (!directModelCopy) return false;
        const auto root = detail::nativePath(m_drive.sectionPath(StoreSection::Models));
        if (!std::filesystem::exists(root / ".iisacc-file-provider/catalog.sqlite3")) return false;
        if (!destinationCatalog)
            destinationCatalog = std::make_unique<iiFileProvider::FileCatalog>(root, iiFileProvider::FileCatalog::Access::ReadOnly);
        const auto relative = iiFileProvider::Directory(m_drive.sectionPath(StoreSection::Models)).relativeFilePath(path);
        if (destinationCatalog->lookupPath(relative.toUtf8().toStdString())) return true;
#ifdef Q_OS_MACOS
        const auto decomposed = relative.normalized(QString::NormalizationForm_D);
        if (decomposed != relative && destinationCatalog->lookupPath(decomposed.toUtf8().toStdString())) return true;
#endif
        return false;
    };
    QString destination;
    const auto parent = QFileInfo(source).absolutePath();
    if (action == Action::Rename) {
        if (argument.isEmpty() || argument.startsWith('.') || argument.contains('/') || argument.contains('\\')
            || argument.contains(':') || argument.contains(QChar::Null)) return failed("Enter a valid file name.");
        destination = iiFileProvider::Directory(parent).filePath(argument);
        if (destination == source) return {source, {}};
    } else {
        const auto folder = action == Action::Copy || action == Action::Move ? argument : parent;
        if (!confined(m_drive, folder, true) || !QFileInfo(folder).isDir()) return failed("Choose a folder in this Society container.");
        if (folder == source || folder.startsWith(source + '/')) return failed("An item cannot be copied into itself.");
        QString moveError;
        destination = action == Action::Move ? iiFileProvider::Directory(folder).filePath(sourceInfo.fileName())
            : uniqueAssetPath(folder, sourceInfo.fileName(), source, action == Action::Duplicate,
                              sourceSection == StoreSection::Models
                                  || (importing && m_drive.sectionForPath(folder) == StoreSection::Models), &moveError, catalogReserved);
        if (destination.isEmpty() && !moveError.isEmpty()) return failed(moveError);
        if (action == Action::Move && destination == source) return {source, {}};
    }
    if (destination.isEmpty() || !editable(destination)) return failed("The destination is unavailable or protected.");
    QList<QPair<QString, QString>> stagedMoves;
    QList<QPair<QString, QString>> copiedAssets;
    QStringList reservedTargets;
    auto cleanup = qScopeGuard([&] { for (const auto &item : stagedMoves) if (QFileInfo::exists(item.first)) removeTree(item.first); });
    if (copy) {
        QString error;
        const bool model = sourceSection == StoreSection::Models
            || (importing && modelPath(m_drive, destination));
        if (!assetMoves(source, destination, model, &copiedAssets, &error, false, &reservedTargets)) return failed(error);
        for (const auto &asset : copiedAssets) {
            const auto stage = QFileInfo(asset.second).absolutePath() + "/.society-copy-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
            stagedMoves.append({stage, asset.second});
            if (!copyTree(asset.first, stage, &error)) return failed(error);
        }
    }
    if (!lock.tryLock(2000)) return failed("Another Society operation is in progress. Try again.");
    if ((importing ? QFileInfo(source).canonicalFilePath() != source : !editable(source)) || !QFileInfo::exists(source)) return failed("The selected item changed. Try again.");
    if (!editable(destination) || QFileInfo::exists(destination) || QFileInfo(destination).isSymLink()) return failed("An item with that name already exists.");
    if (copy) for (const auto &path : reservedTargets) {
        const QFileInfo target(path);
        if (target.exists() || target.isSymLink() || target.isJunction())
            return failed("An asset or its metadata already exists at the destination.");
    }
    if (!copy) relocation = std::make_unique<RelocationNotice>(m_drive.rootPath(), source, destination);
    std::unique_ptr<detail::DeletedOrigins> origins;
    detail::DeletedOrigins::Lookup recorded;
    QString trackedOriginal, originError;
    const auto deletedFolder = m_drive.sectionPath(StoreSection::Deleted);
    if ((action == Action::Rename || action == Action::Move) && source.startsWith(deletedFolder + '/')) {
        origins = std::make_unique<detail::DeletedOrigins>(m_drive);
        QStringList locked{source};
        if (destination.startsWith(deletedFolder + '/')) locked.append(destination);
        if (!origins->lockPaths(locked, &originError)) return failed(originError);
        recorded = origins->find(source);
        if (!recorded.error.isEmpty()) return failed(recorded.error);
        if (!recorded.original.isEmpty() && destination.startsWith(deletedFolder + '/')) {
            trackedOriginal = iiFileProvider::Directory(QFileInfo(recorded.original).absolutePath()).filePath(QFileInfo(destination).fileName());
            if (!origins->write(destination, trackedOriginal, false, &originError)) return failed(originError);
        }
    }
    QList<QPair<QString, QString>> movedGroup;
    if (!copy) {
        const bool model = sourceSection == StoreSection::Models
            || (origins && !recorded.original.isEmpty() && modelPath(m_drive, recorded.original));
        QString moveError;
        if (!assetMoves(source, destination, model, &movedGroup, &moveError, true, &reservedTargets)) return failed(moveError);
        for (const auto &path : reservedTargets) {
            const QFileInfo target(path);
            if (target.exists() || target.isSymLink() || target.isJunction())
                return failed("An asset or its metadata already exists at the destination.");
        }
        bool restored = true;
        if (!m_drive.isReady() || !moveAssets(movedGroup, &moveError, &restored)) {
            if (!trackedOriginal.isEmpty() && restored) origins->forget(destination);
            if (!restored) relocation->retain();
            return restored ? failed(moveError.isEmpty() ? QStringLiteral("The Society drive is unavailable.") : moveError)
                            : Result{QFileInfo::exists(destination) ? destination : source, moveError};
        }
    } else {
        QString moveError; bool restored = true;
        if (!moveAssets(stagedMoves, &moveError, &restored))
            return restored ? failed(moveError) : Result{QFileInfo::exists(destination) ? destination : source, moveError};
    }
    if (!trackedOriginal.isEmpty() && !origins->write(destination, trackedOriginal, true, &originError)) {
        QList<QPair<QString, QString>> reverse;
        for (auto it = movedGroup.crbegin(); it != movedGroup.crend(); ++it) reverse.append({it->second, it->first});
        if (moveAssets(reverse, nullptr, nullptr)) { origins->forget(destination); return failed(originError); }
        if (relocation) relocation->retain();
        return {destination, originError + " The renamed item remains in Deleted; copy it manually to recover it."};
    }
    if (origins && recorded.recordedItem == source) origins->forget(source);
    stagedMoves.clear(); return complete(destination, !copy, copy ? copiedAssets : movedGroup);
    } catch (const std::exception &error) { return failed(QString::fromUtf8(error.what())); }
}
}
