#include "LegacyClassificationMigration.h"
#include "ExclusiveRename.h"
#include "FileTreePaths.h"
#include <FileCatalog.h>
#include <ObjectSource.h>
#include <SharedStorage.h>
#include <StorageMap.h>
#include <iiFileProviderDevice.h>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>
#include <memory>
#include <stdexcept>

namespace iiSocietyContainer::detail {
namespace {
void writeManifest(const QString &path, const QJsonObject &object) {
    QSaveFile out(path); out.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
        throw std::runtime_error("Cannot save the classification recovery record.");
}
QString preserve(const SharedStorage &storage, const QString &source, const QString &logical,
                 const QString &reason, std::stop_token stop) {
    const QFileInfo before(source);
    if (!before.isFile() || before.isSymLink() || before.isJunction() || before.canonicalFilePath() != source)
        throw std::runtime_error("Legacy classification is unavailable or redirected.");
    if (before.size() > 4 * 1024 * 1024)
        throw std::runtime_error("Oversized legacy classification was left in place for manual recovery.");
    QString error;
    const auto vault = storage.ensureRuntimeDirectory("classifications/unresolved", &error);
    if (vault.isEmpty()) throw std::runtime_error(error.toStdString());
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto blob = QDir(vault).filePath(id + ".json");
    const auto manifest = QDir(vault).filePath(id + ".manifest.json");
    const auto held = before.absolutePath() + "/.iifile-quarantine-" + id;
    QJsonObject record{{"format", "iiSocietyContainer.UnresolvedClassification"}, {"schema", 1},
        {"original", logical}, {"claim", held}, {"blob", id + ".json"}, {"reason", reason}, {"state", "pending"}};
    writeManifest(manifest, record);
    if (!renameWithoutCopy(source, held)) throw std::runtime_error("Could not reserve the legacy classification.");
    try {
        const auto identity = iiFileProvider::ObjectSource::inspect(nativePath(held));
        QFile input(held); QSaveFile output(blob); output.setDirectWriteFallback(false);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
            throw std::runtime_error("Could not open the classification recovery file.");
        QCryptographicHash hash(QCryptographicHash::Sha256); QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        for (;;) {
            if (stop.stop_requested()) throw std::runtime_error("Classification migration cancelled.");
            const auto count = input.read(buffer.data(), buffer.size());
            if (count < 0) throw std::runtime_error("Could not read the legacy classification.");
            if (count == 0) break;
            hash.addData(QByteArrayView(buffer.constData(), count));
            if (output.write(buffer.constData(), count) != count)
                throw std::runtime_error("Could not preserve the legacy classification.");
        }
        input.close();
        if (iiFileProvider::ObjectSource::inspect(nativePath(held)) != identity)
            throw std::runtime_error("The legacy classification changed during migration.");
        if (!output.commit()) throw std::runtime_error("Could not publish the preserved classification.");
        if (iiFileProvider::ObjectSource::inspect(nativePath(held)) != identity)
            throw std::runtime_error("The legacy classification changed during publication.");
        if (!QFile::remove(held)) throw std::runtime_error("Could not remove the preserved legacy entry.");
        record.insert("sha256", QString::fromLatin1(hash.result().toHex()));
        record.insert("state", "preserved");
        writeManifest(manifest, record);
    } catch (...) {
        // An independently-created old name is never overwritten. In that case
        // the claim remains beside it and the pending record retains the address.
        if (QFileInfo::exists(held)) (void)renameWithoutCopy(held, source);
        throw;
    }
    return manifest;
}
}

QStringList isolateLegacyClassifications(const SocietyDrive &drive, std::stop_token stop, bool quarantineDetached) {
    QStringList warnings;
    if (!StorageMap(drive).isLocalAuthority()) return warnings;
    QString error; const auto storage = SharedStorage::open(drive.rootPath(), &error);
    if (!storage) throw std::runtime_error(error.toStdString());
    auto repository = storage->fileRepository({}, FileRepository::Access::ReadWrite, FileRepository::Intent::Background);
    const auto lockDirectory = QDir(drive.rootPath()).filePath(".society-sync");
    if (QFileInfo(lockDirectory).isSymLink() || !QDir().mkpath(lockDirectory))
        throw std::runtime_error("The Society operation directory is unavailable.");
    qsizetype seen = 0;
    for (const auto section : allStoreSections()) {
        const auto root = drive.sectionPath(section); const auto key = storeSectionKey(section);
        std::unique_ptr<iiFileProvider::FileCatalog> catalog;
        QStringList directories{root};
        while (!directories.isEmpty()) {
            if (stop.stop_requested()) throw std::runtime_error("Classification migration cancelled.");
            const QDir directory(directories.takeLast());
            for (const auto &entry : directory.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
                if (stop.stop_requested()) throw std::runtime_error("Classification migration cancelled.");
                if (++seen > 250000) throw std::runtime_error("Classification migration exceeded the inventory limit.");
                if (entry.isSymLink() || entry.isJunction()) {
                    if (entry.fileName().endsWith(".iisacc.json", Qt::CaseInsensitive))
                        warnings.append(QStringLiteral("Redirected classification requires manual recovery: %1").arg(entry.absoluteFilePath()));
                    continue;
                }
                if (entry.isDir()) {
                    if (!entry.fileName().startsWith('.') && entry.suffix().compare("iiproject", Qt::CaseInsensitive) != 0
                        && entry.canonicalFilePath() == entry.absoluteFilePath()) directories.append(entry.absoluteFilePath());
                    continue;
                }
                if (!entry.isFile() || !entry.fileName().endsWith(".iisacc.json", Qt::CaseInsensitive)) continue;
                const auto source = entry.absoluteFilePath();
                const auto asset = source.chopped(int(QStringLiteral(".iisacc.json").size()));
                // The ordinary intake can first rebind an externally moved
                // file by its verified native identity. Do not quarantine its
                // old document before that identity reconciliation runs.
                if (!quarantineDetached && !QFileInfo::exists(asset)) continue;
                const auto relative = QDir(root).relativeFilePath(asset);
                const auto logical = key + '/' + QDir(root).relativeFilePath(source);
                bool complete = false;
                while (!complete && !stop.stop_requested()) {
                    iiFileProvider::LockFile operation(QDir(lockDirectory).filePath("operation.lock"));
                    operation.setStaleLockTime(0);
                    QElapsedTimer timer; timer.start();
                    while (!stop.stop_requested() && !operation.tryLock(50))
                        if (timer.elapsed() > 5000) throw std::runtime_error("A file operation is preventing classification migration.");
                    if (stop.stop_requested()) throw std::runtime_error("Classification migration cancelled.");
                    complete = repository->tryBackgroundOperation([&](std::stop_token token) {
                        if (token.stop_requested() || !QFileInfo::exists(source)) return;
                        QString problem;
                        try {
                            if (!catalog) catalog = std::make_unique<iiFileProvider::FileCatalog>(nativePath(root),
                                iiFileProvider::FileCatalog::Access::Create,
                                FileRepository::classificationDirectory(nativePath(drive.rootPath()), key.toStdString()));
                            const QFileInfo target(asset);
                            if (target.isSymLink() || target.isJunction() || target.canonicalFilePath() != asset)
                                throw std::runtime_error("The associated asset cannot be identified safely.");
                            // The pathname alone cannot establish that this is still
                            // the registered object after an external replacement.
                            const auto old = catalog->lookupPath(relative.toUtf8().toStdString());
                            if (!old) throw std::runtime_error("The classification owner has no verified catalog identity.");
                            if (target.isFile()) (void)catalog->read(old->key, 0, 1);
                            else if (target.isDir()) (void)catalog->resolvePackage(old->key);
                            iiFileProvider::MetadataWrite result;
                            if (target.isFile()) result = catalog->registerFile(relative.toUtf8().toStdString());
                            else if (target.isDir() && target.suffix().compare("iiproject", Qt::CaseInsensitive) == 0)
                                result = catalog->registerPackage(relative.toUtf8().toStdString());
                            else throw std::runtime_error("The associated asset is missing or unsupported.");
                            if (!result.metadataWritten) throw std::runtime_error(result.metadataError);
                            if (QFileInfo::exists(source)) throw std::runtime_error("The legacy classification is still present.");
                            return;
                        } catch (const std::exception &failure) { problem = QString::fromUtf8(failure.what()); }
                        if (token.stop_requested()) return;
                        try {
                            (void)preserve(*storage, source, logical, problem, token);
                        } catch (const std::exception &failure) {
                            if (token.stop_requested()) throw;
                            warnings.append(QStringLiteral("Could not isolate %1; recovery is required: %2")
                                .arg(logical, QString::fromUtf8(failure.what())));
                        }
                    }, stop);
                }
            }
        }
    }
    // Unresolved items remain visible to the job/notification consumer on
    // every later reconciliation until an explicit recovery removes the
    // record. Successful isolation is not a successful identity match.
    if (!quarantineDetached) return warnings;
    QString vaultError;
    const auto vaultPath = storage->runtimePath("classifications/unresolved", &vaultError);
    if (vaultPath.isEmpty()) throw std::runtime_error(vaultError.toStdString());
    const QDir vault(vaultPath);
    for (const auto &entry : vault.entryInfoList({"*.manifest.json"}, QDir::Files | QDir::NoSymLinks, QDir::Name)) {
        QFile file(entry.absoluteFilePath());
        if (entry.size() > 65536 || !file.open(QIODevice::ReadOnly)) {
            warnings.append(QStringLiteral("An unresolved classification record cannot be read: %1").arg(entry.absoluteFilePath()));
            continue;
        }
        const auto document = QJsonDocument::fromJson(file.readAll()).object();
        if (document.value("format").toString() != "iiSocietyContainer.UnresolvedClassification"
            || document.value("schema").toInt() != 1 || document.value("original").toString().isEmpty()) {
            warnings.append(QStringLiteral("An unresolved classification record is invalid: %1").arg(entry.absoluteFilePath()));
            continue;
        }
        const auto blobName = document.value("blob").toString();
        const QFileInfo blob(vault.filePath(blobName));
        const bool preserved = document.value("state").toString() == "preserved"
            && !blobName.isEmpty() && QFileInfo(blobName).fileName() == blobName
            && blob.isFile() && !blob.isSymLink() && !blob.isJunction();
        warnings.append((preserved
            ? QStringLiteral("Unresolved classification %1 is preserved at %2: %3")
            : QStringLiteral("Unresolved classification %1 requires recovery from %2: %3"))
            .arg(document.value("original").toString(), entry.absoluteFilePath(), document.value("reason").toString()));
    }
    return warnings;
}
}
