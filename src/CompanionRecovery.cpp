#include "CompanionRecovery.h"
#include "AssetCompanions.h"
#include "ExclusiveRename.h"
#include <FileCatalog.h>
#include <iiFileProviderPresentation.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QUuid>
#include <filesystem>
#include <memory>

namespace iiSocietyContainer::detail {
namespace {
std::filesystem::path native(const QString &p) { return std::filesystem::path(p.toStdU16String()); }
bool occupied(const QFileInfo &p) { return p.exists() || p.isSymLink() || p.isJunction(); }
bool safeRelative(const QString &relative) {
    if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.contains('\\')
        || relative.contains(':') || relative.contains(QChar::Null)) return false;
    for (const auto &part : relative.split('/')) if (part.isEmpty() || part.startsWith('.')) return false;
    return true;
}
QString safePath(const QString &root, const QString &relative) {
    if (!safeRelative(relative)) return {};
    QString path = root;
    const auto parts = relative.split('/');
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (parts[i].isEmpty() || parts[i].startsWith('.')) return {};
        path += '/' + parts[i]; const QFileInfo info(path);
        if (info.isSymLink() || info.isJunction()) return {};
        if (info.exists() && info.canonicalFilePath() != path) return {};
        if (!info.exists() && i != parts.size() - 1) return {};
        if (i != parts.size() - 1 && !info.isDir()) return {};
    }
    return path;
}
QJsonObject readObject(const QString &path, qint64 maximum, QByteArray *raw = nullptr) {
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.isJunction() || info.size() < 1 || info.size() > maximum) return {};
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.readAll(); QJsonParseError error;
    const auto value = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !value.isObject()) return {};
    if (raw) *raw = bytes;
    return value.object();
}
bool sameClassification(const QJsonObject &a, const QJsonObject &b) {
    return a.value("format").toString() == "iiFileProvider.Metadata" && a.value("schema").toInt() == 1
        && a.value("fileKey").isString() && a.value("fileKey") == b.value("fileKey")
        && a.value("metadata").isObject() && a.value("metadata") == b.value("metadata")
        && a.value("collections").isArray() && a.value("collections") == b.value("collections")
        && a.value("revision").toInteger(-1) > 0 && a.value("revision").toInteger(-1) <= b.value("revision").toInteger(-1);
}
bool compatiblePreview(const QString &oldAsset, const QString &target, QString *error) {
    const auto oldPreview = portablePreviewPath(oldAsset), targetPreview = portablePreviewPath(target);
    if (!occupied(QFileInfo(oldPreview))) return true;
    if (!verifyPortablePreview(oldPreview, error)) return false;
    if (!occupied(QFileInfo(targetPreview))) return true;
    if (!verifyPortablePreview(targetPreview, error)) return false;
    if (readObject(oldPreview, 2 * 1024 * 1024).value("preview")
        != readObject(targetPreview, 2 * 1024 * 1024).value("preview")) {
        if (error) *error = QStringLiteral("Preview conflict at %1.").arg(target);
        return false;
    }
    return true;
}
QString aliasTarget(const QJsonObject &aliases, QString path) {
    QSet<QString> visited;
    for (int i = 0; i < 64 && aliases.contains(path); ++i) {
        if (visited.contains(path) || !aliases.value(path).isString()) return {};
        visited.insert(path); path = aliases.value(path).toString();
    }
    return visited.isEmpty() || aliases.contains(path) ? QString() : path;
}
bool archive(const QString &root, const QString &source, const QByteArray &observed, QString *error) {
    const auto vault = root + "/.society-recovered-companions";
    QFileInfo info(vault);
    if (info.isSymLink() || info.isJunction() || (!info.exists() && !QDir().mkdir(vault))) {
        if (error) *error = QStringLiteral("The companion recovery folder is unavailable."); return false;
    }
    info.refresh();
    if (!info.isDir() || info.isSymLink() || info.isJunction() || info.canonicalFilePath() != vault) {
        if (error) *error = QStringLiteral("The companion recovery folder is redirected."); return false;
    }
    const auto destination = vault + '/' + QUuid::createUuid().toString(QUuid::WithoutBraces) + '-' + QFileInfo(source).fileName();
    if (!renameWithoutCopy(source, destination)) { if (error) *error = QStringLiteral("Could not preserve the old companion."); return false; }
    QFile check(destination);
    if (!check.open(QIODevice::ReadOnly) || check.readAll() != observed) {
        check.close();
        const bool restored = renameWithoutCopy(destination, source);
        if (error) *error = restored ? QStringLiteral("The companion changed during recovery.")
                                    : QStringLiteral("The companion changed during recovery; it was preserved at %1.").arg(destination);
        return false;
    }
    return true;
}
}

QJsonObject modelRecoveryAliases(const SocietyDrive &drive, QString *error) {
    if (error) error->clear();
    const auto path = drive.sectionPath(StoreSection::Models) + "/.model-paths.json";
    const QFileInfo info(path);
    if (!occupied(info)) return {};
    const auto document = readObject(path, 4 * 1024 * 1024);
    if (document.value("schemaVersion").toInt() != 1 || document.value("containerId").toString() != drive.identifier()
        || !document.value("aliases").isObject()) {
        if (error) *error = QStringLiteral("The model organization journal is invalid or redirected."); return {};
    }
    const auto aliases = document.value("aliases").toObject();
    for (auto it = aliases.begin(); it != aliases.end(); ++it)
        if (!safeRelative(it.key()) || !it.value().isString() || !safeRelative(it.value().toString())) {
            if (error) *error = QStringLiteral("The model organization journal contains an invalid path."); return {};
        }
    return aliases;
}

QStringList repairModelCompanions(const SocietyDrive &drive, const QJsonObject &aliases,
                                  QStringList *errors, const std::atomic_bool *cancelled, std::stop_token stop) {
    QStringList repaired;
    const auto root = drive.sectionPath(StoreSection::Models);
    const auto catalogFile = root + "/.iisacc-file-provider/catalog.sqlite3";
    std::unique_ptr<iiFileProvider::FileCatalog> catalog;
    if (QFileInfo::exists(catalogFile))
        catalog = std::make_unique<iiFileProvider::FileCatalog>(native(root), iiFileProvider::FileCatalog::Access::ReadWrite);
    QStringList pending{root}; qsizetype seen = 0;
    while (!pending.isEmpty()) {
        if (stop.stop_requested() || (cancelled && cancelled->load())) return repaired;
        const QDir directory(pending.takeLast());
        const auto children = directory.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        for (const auto &child : children) {
            if (stop.stop_requested() || (cancelled && cancelled->load())) return repaired;
            if (++seen > 250000) { if (errors) errors->append(QStringLiteral("Companion recovery exceeded the file inventory limit.")); return repaired; }
            if (child.isSymLink() || child.isJunction()) continue;
            if (child.isDir()) {
                if (!child.fileName().startsWith('.') && child.suffix().compare("iiproject", Qt::CaseInsensitive) != 0
                    && child.canonicalFilePath() == child.absoluteFilePath()) pending.append(child.absoluteFilePath());
                continue;
            }
            if (isPortablePreviewPath(child.absoluteFilePath())) {
                const auto name = child.fileName().mid(1).chopped(int(QStringLiteral(".iisacc-preview").size()));
                const auto asset = directory.filePath(name);
                if (occupied(QFileInfo(child.absoluteFilePath())) && !occupied(QFileInfo(asset))
                    && !occupied(QFileInfo(asset + ".iisacc.json")) && errors)
                    errors->append(QStringLiteral("Detached preview cannot be identified safely and was preserved at %1.").arg(child.absoluteFilePath()));
                continue;
            }
            if (!child.fileName().endsWith(".iisacc.json", Qt::CaseInsensitive)) continue;
            const auto oldAsset = child.absoluteFilePath().chopped(int(QStringLiteral(".iisacc.json").size()));
            if (occupied(QFileInfo(oldAsset))) continue;
            const auto oldRelative = QDir(root).relativeFilePath(oldAsset);
            if (safePath(root, oldRelative) != oldAsset) continue;
            QByteArray oldBytes;
            const auto oldDocument = readObject(child.absoluteFilePath(), qint64(iiFileProvider::FileCatalog::maximumMetadataBytes), &oldBytes);
            const auto key = oldDocument.value("fileKey").toString();
            if (oldDocument.value("format").toString() != "iiFileProvider.Metadata" || oldDocument.value("schema").toInt() != 1
                || key.isEmpty() || oldDocument.value("path").toString() != oldRelative) {
                if (errors) errors->append(QStringLiteral("Detached classification cannot be verified and was preserved at %1.").arg(child.absoluteFilePath()));
                continue;
            }
            if (!catalog) {
                if (errors) errors->append(QStringLiteral("No model catalog can verify the detached classification at %1.").arg(child.absoluteFilePath()));
                continue;
            }
            try {
                auto record = catalog->lookup(key.toStdString());
                if (!record) {
                    if (errors) errors->append(QStringLiteral("No catalog identity matches the detached classification at %1.").arg(child.absoluteFilePath()));
                    continue;
                }
                auto targetRelative = QString::fromStdString(record->path);
                if (targetRelative == oldRelative) targetRelative = aliasTarget(aliases, oldRelative);
                const auto target = safePath(root, targetRelative);
                if (target.isEmpty() || !QFileInfo(target).isFile()) {
                    if (errors) errors->append(QStringLiteral("The moved asset for %1 cannot be identified safely.").arg(child.absoluteFilePath()));
                    continue;
                }
                const QFileInfo targetInfo(target);
                if (targetInfo.size() < 0 || quint64(targetInfo.size()) != record->size
                    || targetInfo.lastModified().toMSecsSinceEpoch() != record->modifiedAtNs / 1000000) {
                    if (errors) errors->append(QStringLiteral("The moved asset changed since the catalog observation at %1.").arg(target));
                    continue;
                }
                iiFileProvider::native::Presentation::cancelPending(native(target));
                QString previewProblem;
                if (!compatiblePreview(oldAsset, target, &previewProblem)) {
                    if (errors) errors->append(previewProblem); continue;
                }
                auto current = catalog->lookupPath(targetRelative.toStdString());
                if (current && current->key != record->key) {
                    if (errors) errors->append(QStringLiteral("Companion identity conflicts at %1.").arg(target)); continue;
                }
                if (!current) {
                    const auto registered = catalog->registerFiles({{targetRelative.toStdString(), {}}});
                    if (registered.size() != 1 || registered.front().key != record->key) {
                        if (errors) errors->append(QStringLiteral("Could not verify the moved asset at %1.").arg(target)); continue;
                    }
                    current = registered.front();
                }
                // read() rechecks the stored native file identity and reads at most one byte.
                (void)catalog->read(current->key, 0, 1);
                const auto expected = QJsonDocument::fromJson(QByteArray::fromStdString(catalog->metadataDocument(current->key))).object();
                if (!sameClassification(oldDocument, expected)) {
                    if (errors) errors->append(QStringLiteral("The old classification differs from the current asset at %1.").arg(target)); continue;
                }
                const auto targetMetadata = target + ".iisacc.json";
                QString problem;
                const bool metadataExists = occupied(QFileInfo(targetMetadata));
                if (metadataExists) {
                    const auto currentDocument = QJsonDocument::fromJson(QByteArray::fromStdString(
                        iiFileProvider::FileCatalog::classificationDocument(native(target)))).object();
                    if (!sameClassification(oldDocument, currentDocument)) {
                        if (errors) errors->append(QStringLiteral("Classification conflict at %1.").arg(target));
                        continue;
                    }
                }
                // Complete or preserve the preview first. If it conflicts or
                // cannot move, the old classification stays in place so a
                // later repair still discovers the entire incomplete asset.
                const auto oldPreview = portablePreviewPath(oldAsset), targetPreview = portablePreviewPath(target);
                if (occupied(QFileInfo(oldPreview))) {
                    QByteArray oldPreviewBytes; QString previewError;
                    const auto oldPreviewDoc = readObject(oldPreview, 2 * 1024 * 1024, &oldPreviewBytes);
                    if (!verifyPortablePreview(oldPreview, &previewError)) { if (errors) errors->append(previewError); continue; }
                    if (occupied(QFileInfo(targetPreview))) {
                        const auto currentPreview = readObject(targetPreview, 2 * 1024 * 1024);
                        if (!verifyPortablePreview(targetPreview, &previewError)
                            || oldPreviewDoc.value("preview") != currentPreview.value("preview")
                            || !archive(root, oldPreview, oldPreviewBytes, &previewError)) {
                            if (errors) errors->append(previewError.isEmpty() ? QStringLiteral("Preview conflict at %1.").arg(target) : previewError);
                            continue;
                        }
                    } else if (!renameWithoutCopy(oldPreview, targetPreview)) {
                        if (errors) errors->append(QStringLiteral("Could not recover the preview for %1.").arg(target));
                        continue;
                    }
                }
                if (metadataExists) {
                    if (!archive(root, child.absoluteFilePath(), oldBytes, &problem)) {
                        if (errors) errors->append(problem); continue;
                    }
                } else {
                    if (!renameWithoutCopy(child.absoluteFilePath(), targetMetadata)) {
                        if (errors) errors->append(QStringLiteral("Could not recover classification for %1.").arg(target)); continue;
                    }
                    try { (void)iiFileProvider::FileCatalog::classificationDocument(native(target)); }
                    catch (...) {
                        const bool restored = renameWithoutCopy(targetMetadata, child.absoluteFilePath());
                        if (errors) errors->append(restored ? QStringLiteral("The old classification was invalid; it was preserved.")
                                                          : QStringLiteral("The classification requires inspection at %1.").arg(targetMetadata));
                        continue;
                    }
                }
                repaired.append(target);
            } catch (const std::exception &e) {
                if (errors) errors->append(QStringLiteral("Could not verify %1: %2").arg(oldAsset, QString::fromUtf8(e.what())));
            }
        }
    }
    return repaired;
}
}
