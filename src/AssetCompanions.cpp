#include "AssetCompanions.h"
#include "ExclusiveRename.h"
#include "FileTreePaths.h"
#include <ModelClassifier.h>
#include <FileOperations.h>
#include <SharedStorage.h>
#include <FileCatalog.h>
#include <ObjectSource.h>
#include <iiFileProviderDevice.h>
#include <iiFileProviderNativeDevice.h>
#include <iiFileProviderPresentation.h>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QDir>
#include <algorithm>
#include <filesystem>
#include <vector>

namespace iiSocietyContainer::detail {
namespace {
void fail(QString *error, const QString &message) { if (error) *error = message; }
std::filesystem::path native(const QString &path) { return std::filesystem::path(path.toStdU16String()); }
QString previewPath(const QString &asset) {
    return QString::fromStdU16String(iiFileProvider::native::Presentation::companionPath(native(asset)).u16string());
}
bool present(const QFileInfo &info) { return info.exists() || info.isSymLink() || info.isJunction(); }
// Empty means the asset is outside a Society container. No public asset
// directory is used to hold private classification documents.
QString privateClassificationPath(const QString &asset, bool createParent, QString *error) {
    const auto drive = FileOperations::containingDrive(asset);
    if (!drive) return {};
    const auto logical = drive->relativePath(asset);
    for (const auto section : allStoreSections()) {
        const auto name = storeSectionName(section);
        if (!logical.startsWith(name + '/')) continue;
        const auto relative = logical.mid(name.size() + 1);
        if (relative.isEmpty()) return {};
        auto target = FileRepository::classificationDirectory(native(drive->rootPath()), storeSectionKey(section).toStdString()) / native(relative);
        target += ".iisacc.json";
        const auto result = QString::fromStdU16String(target.u16string());
        if (createParent) {
            QString problem;
            const auto storage = SharedStorage::open(drive->rootPath(), &problem);
            const auto parent = QFileInfo(relative).path();
            auto directory = QStringLiteral("classifications/") + storeSectionKey(section);
            if (parent != ".") directory += '/' + parent;
            if (!storage || storage->ensureRuntimeDirectory(directory, &problem).isEmpty()) {
                fail(error, problem.isEmpty() ? QStringLiteral("Private classification storage is unavailable.") : problem);
                return {};
            }
        }
        return result;
    }
    return {};
}
bool safePrivateTree(const QString &path) {
    const QFileInfo info(path);
    if (!info.isDir() || info.isSymLink() || info.isJunction() || info.canonicalFilePath() != path) return false;
    const QDir directory(path);
    for (const auto &child : directory.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (child.isSymLink() || child.isJunction()) return false;
        if (child.isDir() && !safePrivateTree(child.absoluteFilePath())) return false;
        if (!child.isDir() && !child.isFile()) return false;
    }
    return true;
}
bool validPreview(const QString &path, QString *error) {
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.isJunction()
        || info.size() < 1 || info.size() > qint64(iiFileProvider::native::Presentation::maximumDocumentBytes)) {
        fail(error, QStringLiteral("The asset preview is unavailable, oversized, or redirected.")); return false;
    }
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) { fail(error, QStringLiteral("Could not read the asset preview.")); return false; }
    QJsonParseError parse;
    const auto json = QJsonDocument::fromJson(input.readAll(), &parse).object();
    const auto details = json.value("details").toObject();
    const auto preview = json.value("preview").toObject();
    const auto encoded = preview.value("data").toString().toLatin1();
    const auto decoded = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (parse.error != QJsonParseError::NoError || json.value("format").toString() != "iiFileProvider.Presentation"
        || json.value("schema").toInt() != 1
        || details.value("name").toString().isEmpty()
        || preview.value("encoding").toString() != "base64" || decoded.isEmpty()
        || decoded.size() > qint64(iiFileProvider::native::Presentation::maximumDocumentBytes / 2)
        || QString::fromLatin1(QCryptographicHash::hash(decoded, QCryptographicHash::Sha256).toHex()) != preview.value("sha256").toString()) {
        fail(error, QStringLiteral("The asset preview cannot be verified. Its original was kept.")); return false;
    }
    return true;
}
bool validProjectClassification(const QString &source, const QString &metadata, QString *error) {
    const QFileInfo manifest(QDir(source).filePath("manifest.json"));
    const QFileInfo before(metadata);
    if (!manifest.isFile() || manifest.isSymLink() || manifest.isJunction() || before.size() <= 0
        || before.size() > qint64(iiFileProvider::FileCatalog::maximumMetadataBytes)) {
        fail(error, QStringLiteral("The project classification is unavailable or oversized.")); return false;
    }
    QFile file(metadata);
    if (!file.open(QIODevice::ReadOnly)) { fail(error, QStringLiteral("Could not read the project classification.")); return false; }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse).object();
    const QFileInfo after(metadata);
    const auto key = document.value("fileKey").toString();
    static const QRegularExpression keyPattern(QStringLiteral("^file-[0-9a-f]{32}$"));
    if (parse.error != QJsonParseError::NoError || document.value("format").toString() != "iiFileProvider.Metadata"
        || document.value("schema").toInt() != 1 || !keyPattern.match(key).hasMatch()
        || document.value("revision").toInteger() <= 0 || !document.value("metadata").isObject()
        || !document.value("collections").isArray() || before.size() != after.size()
        || before.lastModified() != after.lastModified() || before.metadataChangeTime() != after.metadataChangeTime()) {
        fail(error, QStringLiteral("The project classification cannot be verified.")); return false;
    }
    try {
        const auto drive = FileOperations::containingDrive(source);
        const auto section = drive ? drive->sectionForPath(source) : std::nullopt;
        if (!section) { fail(error, QStringLiteral("The project catalog is unavailable.")); return false; }
        const auto root = drive->sectionPath(*section);
        iiFileProvider::FileCatalog catalog(native(root), iiFileProvider::FileCatalog::Access::ReadOnly);
        const auto entry = catalog.lookupPath(QDir(root).relativeFilePath(source).toUtf8().toStdString());
        if (!entry || QString::fromStdString(entry->key) != key
            || quint64(document.value("revision").toInteger()) > entry->revision) {
            fail(error, QStringLiteral("The project classification belongs to another item or revision.")); return false;
        }
        QJsonParseError catalogParse;
        const auto trusted = QJsonDocument::fromJson(QByteArray::fromStdString(catalog.metadataDocument(entry->key)), &catalogParse).object();
        if (catalogParse.error != QJsonParseError::NoError || trusted.value("metadata") != document.value("metadata")
            || trusted.value("collections") != document.value("collections")) {
            fail(error, QStringLiteral("The project classification differs from its catalog record.")); return false;
        }
    } catch (const std::exception &e) {
        fail(error, QStringLiteral("Could not validate project classification: %1").arg(QString::fromUtf8(e.what()))); return false;
    }
    return true;
}
}

bool isPortablePreviewPath(const QString &path) {
    const auto name = QFileInfo(path).fileName();
    return name.startsWith('.') && name.endsWith(".iisacc-preview", Qt::CaseInsensitive);
}
QString portablePreviewPath(const QString &asset) { return previewPath(asset); }
bool verifyPortablePreview(const QString &path, QString *error) { return validPreview(path, error); }

bool appendAssetCompanions(const QString &source, const QString &destination,
                           QList<QPair<QString, QString>> *moves, QString *error) {
    if (!moves) { fail(error, QStringLiteral("The asset companion plan is unavailable.")); return false; }
    const QFileInfo asset(source);
    const auto privateSource = privateClassificationPath(source, false, error);
    const auto legacySource = source + ".iisacc.json";
    if (!privateSource.isEmpty() && present(QFileInfo(privateSource)) && present(QFileInfo(legacySource))) {
        fail(error, QStringLiteral("Both private and legacy classifications exist; resolve the conflict before moving this asset."));
        return false;
    }
    const auto metadata = !privateSource.isEmpty() && present(QFileInfo(privateSource)) ? privateSource : legacySource;
    if (present(QFileInfo(metadata))) {
        const QFileInfo info(metadata);
        if (!info.isFile() || info.isSymLink() || info.isJunction()
            || (!asset.isFile() && !(asset.isDir() && asset.suffix().compare("iiproject", Qt::CaseInsensitive) == 0))) {
            fail(error, QStringLiteral("The asset classification is unavailable or redirected.")); return false;
        }
        if (asset.isDir()) {
            if (!validProjectClassification(source, metadata, error)) return false;
        } else try {
            if (iiFileProvider::FileCatalog::classificationDocumentAt(native(source), native(metadata)).empty()) {
                fail(error, QStringLiteral("The asset classification is unavailable.")); return false;
            }
        } catch (const std::exception &e) {
            fail(error, QStringLiteral("Could not validate asset classification: %1").arg(QString::fromUtf8(e.what()))); return false;
        }
        const auto privateDestination = privateClassificationPath(destination, true, error);
        if (FileOperations::containingDrive(destination) && privateDestination.isEmpty()) return false;
        moves->append({metadata, privateDestination.isEmpty() ? destination + ".iisacc.json" : privateDestination});
    }
    // Directory operations carry their private descendant documents as one
    // separate tree; the public directory still contains only user files.
    if (asset.isDir() && !privateSource.isEmpty()) {
        const auto sourceTree = privateSource.chopped(int(QStringLiteral(".iisacc.json").size()));
        if (present(QFileInfo(sourceTree))) {
            if (!safePrivateTree(sourceTree)) { fail(error, QStringLiteral("A private classification directory is redirected.")); return false; }
            const auto privateDestination = privateClassificationPath(destination, true, error);
            if (privateDestination.isEmpty()) { fail(error, QStringLiteral("Private classification destination is unavailable.")); return false; }
            moves->append({sourceTree, privateDestination.chopped(int(QStringLiteral(".iisacc.json").size()))});
        }
    }
    if (asset.isFile()) {
        const auto preview = previewPath(source);
        if (present(QFileInfo(preview))) {
            if (!validPreview(preview, error)) return false;
            moves->append({preview, previewPath(destination)});
        }
    }
    return true;
}

std::optional<AssetObject::Plan> AssetObject::plan(const QString &source, const QString &destination,
                                                   bool model, bool stabilize, QString *error) {
    Plan result;
    result.assets.append({source, destination});
    if (model && QFileInfo(source).isFile()) {
        for (const auto &companion : ModelClassifier::companionFiles(source)) {
            const bool full = companion.startsWith(source + '.');
            const auto oldPrefix = full ? source : QDir(QFileInfo(source).absolutePath()).filePath(QFileInfo(source).completeBaseName());
            const auto newPrefix = full ? destination : QDir(QFileInfo(destination).absolutePath()).filePath(QFileInfo(destination).completeBaseName());
            result.assets.append({companion, newPrefix + companion.mid(oldPrefix.size())});
        }
    }
    result.members = result.assets;
    for (const auto &asset : result.assets) {
        if (stabilize && QFileInfo(asset.first).isFile())
            iiFileProvider::native::Presentation::cancelPending(nativePath(asset.first));
        if (!appendAssetCompanions(asset.first, asset.second, &result.members, error)) return {};
        // Even when the source has no sidecar, a pre-existing destination
        // sidecar must not be adopted as metadata for an unrelated asset.
        result.reservedTargets.append(asset.second + ".iisacc.json");
        const auto privateTarget = privateClassificationPath(asset.second, false, error);
        if (!privateTarget.isEmpty()) result.reservedTargets.append(privateTarget);
        if (QFileInfo(asset.first).isFile()) result.reservedTargets.append(previewPath(asset.second));
    }
    for (const auto &member : result.members) result.reservedTargets.append(member.second);
    result.reservedTargets.removeDuplicates();
    return result;
}

QString AssetObject::publishImport(iiFileProvider::TemporaryFile &payload, const QString &source,
                                   const QString &directory, const QString &name,
                                   const std::shared_ptr<std::atomic_bool> &cancelled, QString *error,
                                   ImportFailure *failure, const std::function<void()> &waiting,
                                   const std::function<bool(const QString &)> &catalogReserved) {
    if (failure) *failure = ImportFailure::None;
    struct Stage {
        QString destination;
        std::unique_ptr<iiFileProvider::TemporaryFile> file;
        std::optional<iiFileProvider::ObjectSourceIdentity> publishedIdentity;
    };
    const auto stopped = [&] { return cancelled && cancelled->load(); };
    const QFileInfo original(name);
    const auto models = QFileInfo(payload.fileName()).absolutePath();
    const auto root = QFileInfo(models).absolutePath();
    const auto sync = QDir(root).filePath(".society-sync");
    if (QFileInfo(sync).isSymLink() || QFileInfo(sync).isJunction() || !QDir().mkpath(sync)
        || QFileInfo(sync).canonicalFilePath() != sync) {
        fail(error, QStringLiteral("The container operation directory is unavailable or redirected.")); return {};
    }
    for (int index = 0; index < 10000 && !stopped(); ++index) {
        const auto candidate = QDir(directory).filePath(index == 0 ? name
            : QString("%1 (%2).%3").arg(original.completeBaseName()).arg(index).arg(original.suffix()));
        Plan group;
        if (!source.isEmpty()) {
            const auto planned = plan(source, candidate, true, false, error);
            if (!planned) return {};
            group = *planned;
        } else {
            group.reservedTargets = {candidate, candidate + ".iisacc.json", previewPath(candidate)};
        }
        const auto occupied = [](const QString &path) {
            const QFileInfo info(path); return info.exists() || info.isSymLink() || info.isJunction();
        };
        const auto reservedIdentity = [&] {
            if (!catalogReserved) return false;
            if (catalogReserved(candidate)) return true;
            return std::any_of(group.assets.cbegin(), group.assets.cend(), [&](const auto &asset) {
                return asset.second != candidate && catalogReserved(asset.second);
            });
        };
        if (std::any_of(group.reservedTargets.cbegin(), group.reservedTargets.cend(), occupied) || reservedIdentity()) continue;
        std::vector<Stage> stages;
        for (qsizetype i = 1; i < group.members.size(); ++i) {
            if (stopped()) return {};
            const auto &member = group.members[i];
            const QFileInfo before(member.first);
            if (!before.isFile() || before.isSymLink() || before.isJunction()) {
                fail(error, QStringLiteral("A model companion is unavailable or redirected.")); return {};
            }
            // Use the same lease-protected import area and recovery policy as
            // the already-staged primary payload, not arbitrary category folders.
            const auto staging = QFileInfo(payload.fileName()).absolutePath();
            auto file = std::make_unique<iiFileProvider::TemporaryFile>(QDir(staging).filePath(".society-import-XXXXXX"));
            if (!file->open()) { fail(error, QStringLiteral("Could not stage the model metadata.")); return {}; }
            try {
                iiFileProvider::native::Device input(false);
                input.open(nativePath(member.first), {.read = true, .existing = true, .noFollow = true});
                QByteArray buffer(1024 * 1024, Qt::Uninitialized);
                qint64 total = 0;
                while (!stopped()) {
                    const auto count = input.read(buffer.data(), buffer.size());
                    if (count < 0) { fail(error, QStringLiteral("Could not read a model companion.")); return {}; }
                    if (!count) break;
                    if (file->write(buffer.constData(), count) != count) {
                        fail(error, QStringLiteral("Could not stage a model companion.")); return {};
                    }
                    total += count;
                }
                const QFileInfo after(member.first);
                if (stopped()) return {};
                if (!after.isFile() || after.isSymLink() || after.isJunction() || total != before.size()
                    || after.size() != before.size() || after.lastModified() != before.lastModified()
                    || after.metadataChangeTime() != before.metadataChangeTime() || !file->flush()) {
                    fail(error, QStringLiteral("A model companion changed while being copied.")); return {};
                }
            } catch (const std::exception &failure) {
                fail(error, QStringLiteral("Could not stage a model companion: %1").arg(QString::fromUtf8(failure.what())));
                return {};
            }
            file->close();
            stages.push_back({member.second, std::move(file), {}});
        }
        // Detect companions added or removed while the primary was copied.
        if (!source.isEmpty()) {
            const auto checked = plan(source, candidate, true, false, error);
            if (!checked || checked->members != group.members) {
                if (checked) fail(error, QStringLiteral("The model companions changed while being copied."));
                return {};
            }
        }
        // Coordinate the short publication boundary with normal file actions
        // and model category moves. Copying the large payload and companions
        // happens before these locks are taken.
        iiFileProvider::LockFile operation(QDir(sync).filePath("operation.lock"));
        iiFileProvider::LockFile model(QDir(models).filePath(".society-models.lock"));
        const auto acquire = [&](iiFileProvider::LockFile &lock) {
            // A valid owner can legitimately hold the lock for a long time.
            // Only an old, empty, unidentifiable file is eligible for Qt's
            // native-lock-protected stale recovery.
#ifndef Q_OS_ANDROID
            constexpr qint64 abandonedAgeMs = 120000;
#endif
            constexpr qint64 waitLimitMs = 30000;
            QElapsedTimer timer; timer.start();
            bool notified = false;
            while (!stopped()) {
                lock.setStaleLockTime(0);
                if (lock.tryLock(50)) return true;
                if (lock.error() != QLockFile::LockFailedError) {
                    fail(error, QStringLiteral("The container publication lock is unavailable.")); return false;
                }
                if (!notified && waiting) { waiting(); notified = true; }
#ifndef Q_OS_ANDROID
                const QFileInfo info(lock.fileName());
                qint64 pid = 0; QString host, application;
                if (info.isFile() && !info.isSymLink() && !info.isJunction()
                    && info.size() == 0 && info.lastModified().isValid()
                    && info.lastModified().msecsTo(QDateTime::currentDateTimeUtc()) > abandonedAgeMs
                    && !lock.getLockInfo(&pid, &host, &application)) {
                    lock.setStaleLockTime(int(abandonedAgeMs));
                    if (lock.tryLock(0)) return true;
                    lock.setStaleLockTime(0);
                }
#endif
                if (timer.elapsed() >= waitLimitMs) {
                    if (failure) *failure = ImportFailure::LockTimeout;
                    fail(error, QStringLiteral("Another container operation is still using the model publication lock. Retry the import when it finishes."));
                    return false;
                }
            }
            return false;
        };
        if (!acquire(operation) || !acquire(model)) return {};
        if (std::any_of(group.reservedTargets.cbegin(), group.reservedTargets.cend(), occupied) || reservedIdentity()) continue;
        const auto rollback = [&] {
            bool restored = true;
            for (auto it = stages.rbegin(); it != stages.rend(); ++it) {
                if (!it->publishedIdentity) continue;
                try {
                    if (iiFileProvider::ObjectSource::inspect(nativePath(it->destination)) != *it->publishedIdentity
                        || !renameWithoutCopy(it->destination, it->file->fileName())) restored = false;
                } catch (...) { restored = false; }
            }
            return restored;
        };
        bool retry = false;
        for (auto &stage : stages) {
            if (stopped() || !renameWithoutCopy(stage.file->fileName(), stage.destination)) {
                retry = !stopped() && occupied(stage.destination);
                if (!rollback()) {
                    fail(error, QStringLiteral("A model companion could not be rolled back. Preserve the destination and retry manually."));
                    return {};
                }
                if (!retry) {
                    if (!stopped()) fail(error, QStringLiteral("Could not publish the complete model and its companions."));
                    return {};
                }
                break;
            }
            try { stage.publishedIdentity = iiFileProvider::ObjectSource::inspect(nativePath(stage.destination)); }
            catch (const std::exception &failure) {
                fail(error, QStringLiteral("A published companion at %1 needs inspection: %2")
                    .arg(stage.destination, QString::fromUtf8(failure.what())));
                return {};
            }
        }
        if (retry) continue;
        if (!stopped() && payload.rename(candidate)) {
            for (auto &stage : stages) stage.file->setAutoRemove(false);
            payload.setAutoRemove(false);
            return candidate;
        }
        retry = !stopped() && occupied(candidate);
        if (!rollback()) {
            fail(error, QStringLiteral("A model companion could not be rolled back. Preserve the destination and retry manually."));
            return {};
        }
        if (!retry) {
            if (!stopped()) fail(error, QStringLiteral("Could not publish the model without replacing an existing file."));
            return {};
        }
    }
    if (!stopped() && error && error->isEmpty())
        *error = QStringLiteral("No unused name is available for the model and its metadata.");
    return {};
}
}
