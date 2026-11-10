#include "SymbolicLink.h"
#include "FileOperations.h"
#include "StorageMap.h"
#include "SharedStorage.h"
#include "../src/ExclusiveRename.h"
#include "../src/AssetTransfer.h"
#include "../src/DeletedOrigins.h"
#include <FileCatalog.h>
#include <TemporaryLease.h>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QLockFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
using namespace iiSocietyContainer;
class FileOperationsTests : public QObject {
    Q_OBJECT
private slots:
    void movesProjectPackagesAsSingleItems() {
        QTemporaryDir temp(QDir::currentPath() + "/project-package-move-XXXXXX.societycontainer");
        QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        QVERIFY(QDir().mkpath(temp.filePath("Files/Example.iiproject")));
        QFile manifest(temp.filePath("Files/Example.iiproject/manifest.json"));
        QVERIFY(manifest.open(QIODevice::WriteOnly)); QCOMPARE(manifest.write("{}"), 2); manifest.close();
        QVERIFY(!FileOperations(*drive).perform(FileOperations::Action::Duplicate, temp.filePath("Files/Example.iiproject")));
        QVERIFY(!QFileInfo::exists(temp.filePath("Files/Example copy.iiproject")));
        const auto renamed = FileOperations(*drive).perform(FileOperations::Action::Rename,
            temp.filePath("Files/Example.iiproject"), "Renamed.iiproject");
        QVERIFY2(renamed, qPrintable(renamed.error));
        QVERIFY(QFileInfo::exists(renamed.path + "/manifest.json"));
        QVERIFY(!QFileInfo::exists(temp.filePath("Files/Example.iiproject")));
        const auto moved = FileOperations(*drive).perform(FileOperations::Action::Move, renamed.path, temp.filePath("Forked"));
        QVERIFY2(moved, qPrintable(moved.error));
        QVERIFY(QFileInfo::exists(moved.path + "/manifest.json"));
        QFile forged(moved.path + ".iisacc.json"); QVERIFY(forged.open(QIODevice::WriteOnly)); forged.write("{}"); forged.close();
        const auto rejected = FileOperations(*drive).perform(FileOperations::Action::Rename, moved.path, "Other.iiproject");
        QVERIFY(!rejected);
        QVERIFY(QFileInfo::exists(moved.path + "/manifest.json"));
    }
    void relocationParticipantRunsWithinTheMove() {
        QTemporaryDir temp(QDir::currentPath() + "/move-participant-XXXXXX.societycontainer");
        QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        QFile file(temp.filePath("Files/original.txt")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("source"); file.close();
        QList<FileOperations::RelocationPhase> phases;
        FileOperations::setRelocationHandler([&](const QString &, const QString &source, const QString &destination,
                                                  FileOperations::RelocationPhase phase) {
            phases.append(phase);
            if (phase == FileOperations::RelocationPhase::Prepare) {
                QVERIFY(QFileInfo::exists(source)); QVERIFY(!QFileInfo::exists(destination));
            }
            if (phase == FileOperations::RelocationPhase::Commit) {
                QVERIFY(!QFileInfo::exists(source)); QVERIFY(QFileInfo::exists(destination));
            }
        });
        const auto result = FileOperations(*drive).perform(FileOperations::Action::Rename, file.fileName(), "renamed.txt");
        FileOperations::setRelocationHandler({});
        QVERIFY2(result, qPrintable(result.error));
        QCOMPARE(phases, (QList<FileOperations::RelocationPhase>{FileOperations::RelocationPhase::Prepare, FileOperations::RelocationPhase::Commit}));
    }
    void caseOnlyRenameKeepsTheItem() {
        QTemporaryDir temp(QDir::currentPath() + "/case-only-move-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        QFile file(temp.filePath("Files/lower.txt")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("content"); file.close();
        if (!QFileInfo::exists(temp.filePath("Files/LOWER.txt"))) QSKIP("The test filesystem distinguishes case.");
        const auto result = FileOperations(*drive).perform(FileOperations::Action::Rename, file.fileName(), "LOWER.txt");
        QVERIFY2(result, qPrintable(result.error));
        QCOMPARE(QFileInfo(result.path).fileName(), QStringLiteral("LOWER.txt"));
        QFile output(result.path); QVERIFY(output.open(QIODevice::ReadOnly)); QCOMPARE(output.readAll(), QByteArray("content"));
    }
    void relocationFailureReportsWhichSideMoved() {
        QTemporaryDir temp(QDir::currentPath() + "/move-failures-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        QFile file(temp.filePath("Files/original.txt")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("content"); file.close();
        FileOperations::setRelocationHandler([](const QString &, const QString &, const QString &,
                                                FileOperations::RelocationPhase phase) {
            if (phase == FileOperations::RelocationPhase::Prepare) throw std::runtime_error("prepare denied");
        });
        const auto denied = FileOperations(*drive).perform(FileOperations::Action::Rename, file.fileName(), "denied.txt");
        FileOperations::setRelocationHandler({});
        QVERIFY(!denied); QVERIFY(denied.path.isEmpty()); QVERIFY(QFileInfo::exists(file.fileName()));
        QVERIFY(!QFileInfo::exists(temp.filePath("Files/denied.txt")));
        FileOperations::setRelocationHandler([](const QString &, const QString &, const QString &,
                                                FileOperations::RelocationPhase phase) {
            if (phase == FileOperations::RelocationPhase::Commit) throw std::runtime_error("commit interrupted");
        });
        const auto partial = FileOperations(*drive).perform(FileOperations::Action::Rename, file.fileName(), "moved.txt");
        FileOperations::setRelocationHandler({});
        QVERIFY(!partial); QCOMPARE(partial.path, temp.filePath("Files/moved.txt"));
        QVERIFY(QFileInfo::exists(partial.path)); QVERIFY(!QFileInfo::exists(file.fileName()));
    }

    void deletedLocksSeparateIndependentAssetsAndProtectRelatedPaths() {
        QTemporaryDir temp(QDir::currentPath() + "/deleted-lock-scope-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        detail::DeletedOrigins first(*drive), independent(*drive), child(*drive), model(*drive), companion(*drive);
        QString error;
        QVERIFY2(first.lockPaths({temp.filePath("Deleted/package")}, &error, 0), qPrintable(error));
        QVERIFY2(independent.lockPaths({temp.filePath("Deleted/other")}, &error, 0), qPrintable(error));
        QVERIFY(!child.lockPaths({temp.filePath("Deleted/package/nested.bin")}, &error, 0));
        QVERIFY2(model.lockPaths({temp.filePath("Deleted/example.safetensors")}, &error, 0), qPrintable(error));
        QVERIFY(!companion.lockPaths({temp.filePath("Deleted/example.preview.png")}, &error, 0));
        first.unlock();
        QVERIFY2(child.lockPaths({temp.filePath("Deleted/package/nested.bin")}, &error, 0), qPrintable(error));
    }
    void providerControlsCannotBeManipulatedAsFiles() {
        QTemporaryDir temp(QDir::currentPath() + "/protected-metadata-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        const auto files = temp.filePath("Files");
        QFile asset(files + "/document.json"); QVERIFY(asset.open(QIODevice::WriteOnly));
        QCOMPARE(asset.write("{}"), 2); asset.close();
        iiFileProvider::FileCatalog catalog(std::filesystem::path(files.toStdU16String()));
        QVERIFY(catalog.registerFile("document.json").metadataWritten);
        const auto metadata = files + "/document.json.iisacc.json";
        QVERIFY(QFileInfo::exists(metadata));
        FileOperations operations(*drive);
        QVERIFY(!operations.editable(metadata));
        QVERIFY(!operations.perform(FileOperations::Action::Rename, metadata, "exposed.json"));
        QVERIFY(!operations.perform(FileOperations::Action::Trash, metadata));
        QVERIFY(!operations.perform(FileOperations::Action::Copy, metadata, files));
        QVERIFY(!operations.perform(FileOperations::Action::Rename, asset.fileName(), "fake.iisacc.json"));
        QVERIFY(QFileInfo::exists(metadata)); QVERIFY(QFileInfo::exists(asset.fileName()));
        QVERIFY(operations.editable(asset.fileName()));
    }
    void orphanedDestinationMetadataIsNeverAdopted() {
        QTemporaryDir temp(QDir::currentPath() + "/orphan-metadata-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        QTemporaryDir source(QDir::currentPath() + "/orphan-source-XXXXXX"); QVERIFY(source.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        const auto input = source.filePath("asset.txt");
        QFile asset(input); QVERIFY(asset.open(QIODevice::WriteOnly)); QCOMPARE(asset.write("source"), 6); asset.close();
        const auto folder = temp.filePath("Files");
        const auto orphan = folder + "/asset.txt.iisacc.json";
        QFile foreign(orphan); QVERIFY(foreign.open(QIODevice::WriteOnly)); QCOMPARE(foreign.write("unrelated"), 9); foreign.close();
        const auto copied = FileOperations(*drive).perform(FileOperations::Action::Copy, input, folder);
        QVERIFY2(copied, qPrintable(copied.error));
        QCOMPARE(copied.path, folder + "/asset 2.txt");
        QVERIFY(!FileOperations(*drive).perform(FileOperations::Action::Rename, copied.path, "asset.txt"));
        QVERIFY(foreign.open(QIODevice::ReadOnly)); QCOMPARE(foreign.readAll(), QByteArray("unrelated"));
        QVERIFY(QFileInfo::exists(copied.path));
    }
    void stagesAndRecoversCompleteAssetGroupsAcrossVolumes() {
        QTemporaryDir temp(QDir::currentPath() + "/asset-transfer-XXXXXX"); QVERIFY(temp.isValid());
        QVERIFY(QDir().mkdir(temp.filePath("from"))); QVERIFY(QDir().mkdir(temp.filePath("to")));
        const auto source = temp.filePath("from/model.safetensors");
        const auto target = temp.filePath("to/model.safetensors");
        const detail::AssetMoveList moves{{source, target},
            {source + ".iisacc.json", target + ".iisacc.json"},
            {temp.filePath("from/.model.safetensors.iisacc-preview"), temp.filePath("to/.model.safetensors.iisacc-preview")}};
        const auto write = [](const QString &path, const QByteArray &bytes) {
            QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        const auto read = [](const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); };
        const auto copy = [](const QString &from, const QString &to, QString *error) {
            if (QFile::copy(from, to)) return true;
            if (error) *error = QStringLiteral("copy failed"); return false;
        };
        const auto move = [](const detail::AssetMoveList &items, QString *error, bool *restored, int *) {
            if (restored) *restored = true;
            for (const auto &item : items) {
                if (QFileInfo::exists(item.second)) { if (error) *error = QStringLiteral("destination exists"); return false; }
            }
            detail::AssetMoveList completed;
            for (const auto &item : items) {
                if (detail::renameWithoutCopy(item.first, item.second)) { completed.append(item); continue; }
                bool ok = true;
                for (auto it = completed.crbegin(); it != completed.crend(); ++it)
                    ok = detail::renameWithoutCopy(it->second, it->first) && ok;
                if (restored) *restored = ok;
                if (error) *error = QStringLiteral("move failed"); return false;
            }
            return true;
        };
        const auto remove = [](const QString &path) {
            return QFileInfo(path).isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
        };
        const QList<QByteArray> bytes{"model", "metadata", "preview"};
        for (qsizetype i = 0; i < moves.size(); ++i) QVERIFY(write(moves[i].first, bytes[i]));
        bool changed = false;
        const auto copyWhileEditing = [&](const QString &from, const QString &to, QString *error) {
            if (!copy(from, to, error)) return false;
            if (!changed && from == moves[1].first) {
                QFile file(source); if (!file.open(QIODevice::Append) || file.write("!") != 1) return false;
                changed = true;
            }
            return true;
        };
        auto result = detail::transferAssetGroup(moves, copyWhileEditing, move, remove, [](QString *) { return true; });
        QVERIFY(!result.committed); QVERIFY(result.originalsRestored); QVERIFY(result.error.contains("changed"));
        QCOMPARE(read(source), QByteArray("model!"));
        QVERIFY(!QFileInfo::exists(target)); QVERIFY(write(source, bytes[0]));
        QVERIFY(write(target + ".iisacc.json", "other metadata"));
        result = detail::transferAssetGroup(moves, copy, move, remove, [](QString *) { return true; });
        QVERIFY(!result.committed); QVERIFY(result.originalsRestored);
        for (qsizetype i = 0; i < moves.size(); ++i) QCOMPARE(read(moves[i].first), bytes[i]);
        QCOMPARE(read(target + ".iisacc.json"), QByteArray("other metadata"));
        QVERIFY(QFile::remove(target + ".iisacc.json"));
        result = detail::transferAssetGroup(moves, copy, move, remove, [](QString *) { return true; });
        QVERIFY2(result.committed, qPrintable(result.error)); QVERIFY(result.error.isEmpty());
        for (qsizetype i = 0; i < moves.size(); ++i) {
            QVERIFY(!QFileInfo::exists(moves[i].first)); QCOMPARE(read(moves[i].second), bytes[i]);
        }
        QVERIFY(QDir(temp.filePath("from")).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());

        // A failed journal commit preserves both published copies and originals.
        for (qsizetype i = 0; i < moves.size(); ++i) {
            QVERIFY(QFile::remove(moves[i].second)); QVERIFY(write(moves[i].first, bytes[i]));
        }
        result = detail::transferAssetGroup(moves, copy, move, remove, [](QString *error) {
            *error = QStringLiteral("journal failed"); return false;
        });
        QVERIFY(!result.committed); QVERIFY(result.originalsRestored); QVERIFY(result.error.contains("journal failed"));
        for (qsizetype i = 0; i < moves.size(); ++i) {
            QCOMPARE(read(moves[i].first), bytes[i]); QCOMPARE(read(moves[i].second), bytes[i]);
        }
        for (const auto &item : moves) QVERIFY(QFile::remove(item.second));
        QString changedRecovery;
        result = detail::transferAssetGroup(moves, copy, move, remove, [&](QString *error) {
            const QDir parent(temp.filePath("from"));
            for (const auto &name : parent.entryList({".society-recovery-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
                const auto candidate = parent.filePath(name + "/model.safetensors");
                if (!QFileInfo::exists(candidate)) continue;
                QFile file(candidate);
                if (file.open(QIODevice::Append) && file.write("!") == 1) { changedRecovery = candidate; return true; }
            }
            *error = QStringLiteral("test recovery missing"); return false;
        });
        QVERIFY(result.committed); QVERIFY(result.error.contains("changed")); QVERIFY(!changedRecovery.isEmpty());
        QCOMPARE(read(target), bytes[0]); QCOMPARE(read(changedRecovery), QByteArray("model!"));
        QVERIFY(QFileInfo::exists(QFileInfo(changedRecovery).absolutePath() + "/model.safetensors.iisacc.json"));
    }

    void movesAndRestoresFileClassificationWithTheAsset() {
        QTemporaryDir temp(QDir::currentPath() + "/file-object-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        const auto source = temp.filePath("Files/document.txt");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.write("asset bytes") > 0); file.close();
        iiFileProvider::FileCatalog catalog(std::filesystem::path(temp.filePath("Files").toStdU16String()));
        iiFileProvider::FileMetadata metadata; metadata.title = "Important asset";
        QVERIFY(catalog.registerFile("document.txt", metadata).metadataWritten);
        const auto originalCatalog = catalog.lookupPath("document.txt"); QVERIFY(originalCatalog);
        const auto originalProviderKey = originalCatalog->key;
        const auto preview = [](const QString &asset) {
            const QFileInfo info(asset); return info.absolutePath() + "/." + info.fileName() + ".iisacc-preview";
        };
        QVERIFY(QFileInfo::exists(preview(source)));
        const auto storage = SharedStorage::open(temp.path()); QVERIFY(storage);
        QString error; QVERIFY2(storage->recordFile(StoreSection::Files, "document.txt", &error), qPrintable(error));
        const auto before = storage->fileTracking(StoreSection::Files, "document.txt").value("objectKey");
        QVERIFY(!before.toString().isEmpty());
        QVERIFY(QDir().mkpath(temp.filePath("Files/archive")));
        FileOperations operations(*drive);
        const auto moved = operations.perform(FileOperations::Action::Move, source, temp.filePath("Files/archive"));
        QVERIFY2(moved, qPrintable(moved.error));
        QVERIFY(!QFileInfo::exists(source + ".iisacc.json"));
        QVERIFY(QFileInfo::exists(moved.path + ".iisacc.json"));
        QVERIFY(!QFileInfo::exists(preview(source))); QVERIFY(QFileInfo::exists(preview(moved.path)));
        QCOMPARE(storage->fileTracking(StoreSection::Files, "archive/document.txt").value("objectKey"), before);
        const auto trashed = operations.perform(FileOperations::Action::Trash, moved.path);
        QVERIFY2(trashed, qPrintable(trashed.error));
        QVERIFY(!QFileInfo::exists(moved.path + ".iisacc.json"));
        QVERIFY(QFileInfo::exists(trashed.path + ".iisacc.json"));
        QVERIFY(!QFileInfo::exists(preview(moved.path))); QVERIFY(QFileInfo::exists(preview(trashed.path)));
        const auto restored = operations.perform(FileOperations::Action::Restore, trashed.path);
        QVERIFY2(restored, qPrintable(restored.error)); QCOMPARE(restored.path, moved.path);
        QVERIFY(!QFileInfo::exists(trashed.path + ".iisacc.json"));
        QVERIFY(QFileInfo::exists(restored.path + ".iisacc.json"));
        QVERIFY(!QFileInfo::exists(preview(trashed.path))); QVERIFY(QFileInfo::exists(preview(restored.path)));
        QCOMPARE(storage->fileTracking(StoreSection::Files, "archive/document.txt").value("objectKey"), before);
        const auto duplicate = operations.perform(FileOperations::Action::Duplicate, restored.path);
        QVERIFY2(duplicate, qPrintable(duplicate.error));
        QVERIFY(QFileInfo::exists(restored.path + ".iisacc.json"));
        QVERIFY(QFileInfo::exists(duplicate.path + ".iisacc.json"));
        QVERIFY(QFileInfo::exists(preview(restored.path))); QVERIFY(QFileInfo::exists(preview(duplicate.path)));
        const auto duplicateRelative = QDir(temp.filePath("Files")).relativeFilePath(duplicate.path);
        const auto duplicateRecord = storage->fileTracking(StoreSection::Files, duplicateRelative);
        QVERIFY(!duplicateRecord.value("objectKey").toString().isEmpty());
        QVERIFY(duplicateRecord.value("objectKey") != before);
        const auto duplicateCatalog = catalog.lookupPath(duplicateRelative.toStdString());
        QVERIFY(duplicateCatalog); QCOMPARE(QString::fromStdString(duplicateCatalog->metadata.title), QStringLiteral("Important asset"));
        QVERIFY(duplicateCatalog->key != originalProviderKey);
        QFile duplicateMetadata(duplicate.path + ".iisacc.json"); QVERIFY(duplicateMetadata.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(duplicateMetadata.readAll()).object().value("fileKey").toString(),
                 QString::fromStdString(duplicateCatalog->key));
        duplicateMetadata.close();
        const auto duplicateTrash = operations.perform(FileOperations::Action::Trash, duplicate.path);
        QVERIFY2(duplicateTrash, qPrintable(duplicateTrash.error));
        QVERIFY(QFileInfo::exists(duplicateTrash.path + ".iisacc.json"));
        const auto removed = operations.perform(FileOperations::Action::Remove, duplicateTrash.path);
        QVERIFY2(removed, qPrintable(removed.error));
        QVERIFY(!QFileInfo::exists(duplicateTrash.path));
        QVERIFY(!QFileInfo::exists(duplicateTrash.path + ".iisacc.json"));
        QVERIFY(!QFileInfo::exists(preview(duplicateTrash.path)));
        QVERIFY(QFileInfo::exists(restored.path)); QVERIFY(QFileInfo::exists(restored.path + ".iisacc.json"));
    }

    void duplicatesModelAndItsAssociatedAssetWithDistinctIdentity() {
        QTemporaryDir temp(QDir::currentPath() + "/model-object-copy-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        const auto model = temp.filePath("Models/Other/model.safetensors");
        const auto preview = temp.filePath("Models/Other/model.preview.png");
        QVERIFY(QDir().mkpath(QFileInfo(model).absolutePath()));
        for (const auto &path : {model, preview}) {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.write("asset") > 0); file.close();
        }
        iiFileProvider::FileCatalog catalog(std::filesystem::path(temp.filePath("Models").toStdU16String()));
        iiFileProvider::FileMetadata metadata; metadata.title = "Model preview";
        QVERIFY(catalog.registerFile("Other/model.safetensors").metadataWritten);
        QVERIFY(catalog.registerFile("Other/model.preview.png", metadata).metadataWritten);
        const auto storage = SharedStorage::open(temp.path()); QVERIFY(storage); QString error;
        QVERIFY2(storage->recordFile(StoreSection::Models, "Other/model.safetensors", &error), qPrintable(error));
        QVERIFY2(storage->recordFile(StoreSection::Models, "Other/model.preview.png", &error), qPrintable(error));
        const auto original = storage->fileTracking(StoreSection::Models, "Other/model.preview.png").value("resource").toObject().value("id");
        QVERIFY(!original.toString().isEmpty());
        const auto result = FileOperations(*drive).perform(FileOperations::Action::Duplicate, model);
        QVERIFY2(result, qPrintable(result.error));
        const auto copiedPreview = temp.filePath("Models/Other/model copy.preview.png");
        QVERIFY(QFileInfo::exists(copiedPreview)); QVERIFY(QFileInfo::exists(copiedPreview + ".iisacc.json"));
        const auto copiedTracking = storage->fileTracking(StoreSection::Models, "Other/model copy.preview.png");
        QCOMPARE(copiedTracking.value("resourceStatus").toString(), QStringLiteral("working-copy-only"));
        QVERIFY(copiedTracking.value("objectKey").toString().isEmpty());
        const auto copied = copiedTracking.value("resource").toObject().value("id");
        QVERIFY(!copied.toString().isEmpty()); QVERIFY(copied != original);
        const auto copiedCatalog = catalog.lookupPath("Other/model copy.preview.png"); QVERIFY(copiedCatalog);
        QCOMPARE(QString::fromStdString(copiedCatalog->metadata.title), QStringLiteral("Model preview"));
        QFile copiedMetadata(copiedPreview + ".iisacc.json"); QVERIFY(copiedMetadata.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(copiedMetadata.readAll()).object().value("fileKey").toString(),
                 QString::fromStdString(copiedCatalog->key));
        copiedMetadata.close();
        const auto trashed = FileOperations(*drive).perform(FileOperations::Action::Trash, result.path);
        QVERIFY2(trashed, qPrintable(trashed.error));
        const auto deletedPreview = QFileInfo(trashed.path).absolutePath() + "/model copy.preview.png";
        QVERIFY(QFileInfo::exists(deletedPreview));
        const auto restored = FileOperations(*drive).perform(FileOperations::Action::Restore, trashed.path);
        QVERIFY2(restored, qPrintable(restored.error)); QCOMPARE(restored.path, result.path);
        QVERIFY(QFileInfo::exists(copiedPreview)); QVERIFY(QFileInfo::exists(copiedPreview + ".iisacc.json"));
        QVERIFY(!QFileInfo::exists(deletedPreview));
    }

    void importingModelCopiesRecognizedCompanions() {
        QTemporaryDir temp(QDir::currentPath() + "/model-object-import-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        QTemporaryDir external(QDir::currentPath() + "/model-external-XXXXXX"); QVERIFY(external.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        const auto model = external.filePath("model.safetensors");
        const auto description = external.filePath("model.model.json");
        for (const auto &path : {model, description}) {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.write("asset") > 0); file.close();
        }
        iiFileProvider::FileCatalog externalCatalog(std::filesystem::path(external.path().toStdU16String()));
        iiFileProvider::FileMetadata metadata; metadata.title = "Imported description";
        QVERIFY(externalCatalog.registerFile("model.safetensors").metadataWritten);
        QVERIFY(externalCatalog.registerFile("model.model.json", metadata).metadataWritten);
        const auto folder = temp.filePath("Models/Other"); QVERIFY(QDir().mkpath(folder));
        const auto storage = SharedStorage::open(temp.path()); QVERIFY(storage);
        auto repository = storage->fileRepository();
        const auto identity = drive->storageKind() == SocietyDrive::StorageKind::LogicalDisk
            ? drive->storageIdentifier() : drive->identifier();
        const auto package = FileRepository::packagePath(std::filesystem::path(temp.path().toStdU16String()), identity.toStdString());
        iiFileProvider::TemporaryLease lock(package / "operations", iiFileProvider::TemporaryLease::Mode::TryExclusive);
        QVERIFY(lock.locked());
        const auto result = FileOperations(*drive).perform(FileOperations::Action::Copy, model, folder);
        QVERIFY2(result, qPrintable(result.error));
        QVERIFY(lock.locked());
        const auto copiedDescription = folder + "/model.model.json";
        QVERIFY(QFileInfo::exists(result.path + ".iisacc.json"));
        QVERIFY(QFileInfo::exists(copiedDescription)); QVERIFY(QFileInfo::exists(copiedDescription + ".iisacc.json"));
        QVERIFY(QFileInfo::exists(model)); QVERIFY(QFileInfo::exists(description));
        const auto tracking = storage->fileTracking(StoreSection::Models, "Other/model.model.json");
        QCOMPARE(tracking.value("resourceStatus").toString(), QStringLiteral("working-copy-only"));
        QVERIFY(tracking.value("objectKey").toString().isEmpty());
        QVERIFY(!tracking.value("resource").toObject().value("id").toString().isEmpty());
        QVERIFY(!repository->lookup("models/Other/model.safetensors"));
        QVERIFY(!repository->lookup("models/Other/model.model.json"));
        iiFileProvider::FileCatalog destinationCatalog(std::filesystem::path(temp.filePath("Models").toStdU16String()));
        const auto copied = destinationCatalog.lookupPath("Other/model.model.json"); QVERIFY(copied);
        QCOMPARE(QString::fromStdString(copied->metadata.title), QStringLiteral("Imported description"));
    }

    void nativeRenameNeverReplacesAnExistingItem() {
        QTemporaryDir temp(QDir::currentPath() + "/exclusive-rename-XXXXXX"); QVERIFY(temp.isValid());
        const auto source = temp.filePath("source.bin");
        const auto target = temp.filePath("target.bin");
        QFile first(source), second(target);
        QVERIFY(first.open(QIODevice::WriteOnly)); QCOMPARE(first.write("source"), 6); first.close();
        QVERIFY(second.open(QIODevice::WriteOnly)); QCOMPARE(second.write("target"), 6); second.close();
        QVERIFY(!detail::renameWithoutCopy(source, target));
        QVERIFY(first.open(QIODevice::ReadOnly)); QCOMPARE(first.readAll(), QByteArray("source")); first.close();
        QVERIFY(second.open(QIODevice::ReadOnly)); QCOMPARE(second.readAll(), QByteArray("target")); second.close();
        QVERIFY(QFile::remove(target));
        QVERIFY(detail::renameWithoutCopy(source, target));
        QVERIFY(!QFileInfo::exists(source));
        QVERIFY(second.open(QIODevice::ReadOnly)); QCOMPARE(second.readAll(), QByteArray("source"));
    }
    void restoresDeletedItemsWithoutOverwritingAndKeepsOrigins() {
        QTemporaryDir temp(QDir::currentPath() + "/restore-deleted-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive); FileOperations ops(*drive);
        QVERIFY(QDir().mkpath(temp.filePath("Files/projects")));
        const auto original = temp.filePath("Files/projects/example.txt");
        QFile source(original); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("original"); source.close();
        const auto trashed = ops.perform(FileOperations::Action::Trash, original);
        QVERIFY2(trashed, qPrintable(trashed.error)); QVERIFY(!QFileInfo::exists(original));
        QFile replacement(original); QVERIFY(replacement.open(QIODevice::WriteOnly)); replacement.write("replacement"); replacement.close();
        const auto restored = ops.perform(FileOperations::Action::Restore, trashed.path);
        QVERIFY2(restored, qPrintable(restored.error));
        QVERIFY(restored.path != original); QVERIFY(!QFileInfo::exists(trashed.path));
        QFile restoredFile(restored.path); QVERIFY(restoredFile.open(QIODevice::ReadOnly)); QCOMPARE(restoredFile.readAll(), QByteArray("original"));
        QVERIFY(replacement.open(QIODevice::ReadOnly)); QCOMPARE(replacement.readAll(), QByteArray("replacement"));

        QVERIFY(QDir().mkpath(temp.filePath("Models/package/sub")));
        QFile model(temp.filePath("Models/package/sub/model.bin")); QVERIFY(model.open(QIODevice::WriteOnly)); model.write("weights"); model.close();
        const auto package = ops.perform(FileOperations::Action::Trash, temp.filePath("Models/package")); QVERIFY(package);
        const auto child = ops.perform(FileOperations::Action::Restore, package.path + "/sub/model.bin");
        QVERIFY2(child, qPrintable(child.error)); QCOMPARE(child.path, temp.filePath("Models/package/sub/model.bin"));
        QVERIFY(QFileInfo::exists(child.path));

        QFile unknown(temp.filePath("Deleted/unknown.txt")); QVERIFY(unknown.open(QIODevice::WriteOnly)); unknown.write("keep"); unknown.close();
        const auto noOrigin = ops.perform(FileOperations::Action::Restore, unknown.fileName());
        QVERIFY(!noOrigin); QVERIFY(!noOrigin.error.isEmpty()); QVERIFY(QFileInfo::exists(unknown.fileName()));
        QVERIFY(!ops.perform(FileOperations::Action::RestoreTo, unknown.fileName(), temp.filePath("Deleted")));
        QVERIFY(!ops.perform(FileOperations::Action::RestoreTo, unknown.fileName(), QFileInfo(temp.path()).absolutePath()));
        QFile existing(temp.filePath("Files/projects/unknown.txt")); QVERIFY(existing.open(QIODevice::WriteOnly)); existing.write("existing"); existing.close();
        const auto chosen = ops.perform(FileOperations::Action::RestoreTo, unknown.fileName(), temp.filePath("Files/projects"));
        QVERIFY2(chosen, qPrintable(chosen.error)); QVERIFY(chosen.path != existing.fileName());
        QVERIFY(QFileInfo::exists(chosen.path)); QVERIFY(QFileInfo::exists(existing.fileName()));
        QVERIFY(!ops.perform(FileOperations::Action::Restore, original));
    }

    void renamedAndLegacyDeletedItemsKeepTrustworthyOrigins() {
        QTemporaryDir temp(QDir::currentPath() + "/restore-origin-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive); FileOperations ops(*drive);
        const auto storage = SharedStorage::open(temp.path()); QVERIFY(storage);
        QFile legacy(temp.filePath("Files/legacy.txt")); QVERIFY(legacy.open(QIODevice::WriteOnly)); legacy.write("legacy"); legacy.close();
        QString error; QVERIFY2(storage->recordFile(StoreSection::Files, "legacy.txt", &error), qPrintable(error));
        const auto moved = ops.perform(FileOperations::Action::Trash, legacy.fileName()); QVERIFY2(moved, qPrintable(moved.error));
        // A reconciled legacy record carries the current native identity stamp.
        QVERIFY2(storage->recordFile(StoreSection::Deleted, "legacy.txt", &error), qPrintable(error));
        // Simulate a file deleted by an older release, before the recovery journal existed.
        QDir journal(temp.filePath(".society-deleted"));
        for (const auto &name : journal.entryList({"*.json"}, QDir::Files)) QVERIFY(journal.remove(name));
        const auto recovered = ops.perform(FileOperations::Action::Restore, moved.path);
        QVERIFY2(recovered, qPrintable(recovered.error)); QCOMPARE(recovered.path, legacy.fileName());
        const auto again = ops.perform(FileOperations::Action::Trash, recovered.path); QVERIFY(again);
        const auto renamed = ops.perform(FileOperations::Action::Rename, again.path, "renamed.txt"); QVERIFY2(renamed, qPrintable(renamed.error));
        const auto restored = ops.perform(FileOperations::Action::Restore, renamed.path);
        QVERIFY2(restored, qPrintable(restored.error)); QCOMPARE(restored.path, temp.filePath("Files/renamed.txt"));

        const auto protectedItem = ops.perform(FileOperations::Action::Trash, restored.path); QVERIFY(protectedItem);
        QVERIFY(QFile::rename(protectedItem.path, protectedItem.path + ".kept"));
        QFile replacement(protectedItem.path); QVERIFY(replacement.open(QIODevice::WriteOnly)); replacement.write("different"); replacement.close();
        const auto mismatch = ops.perform(FileOperations::Action::Restore, protectedItem.path);
        QVERIFY(!mismatch); QVERIFY(mismatch.error.contains("no longer matches"));
        QVERIFY(QFileInfo::exists(protectedItem.path)); QVERIFY(QFileInfo::exists(protectedItem.path + ".kept"));
        for (const auto &name : journal.entryList({"*.json"}, QDir::Files)) QVERIFY(journal.remove(name));
        const auto stale = ops.perform(FileOperations::Action::Restore, protectedItem.path);
        QVERIFY(!stale); QVERIFY(stale.error.contains("cannot be verified"));
        QVERIFY(QFileInfo::exists(protectedItem.path)); QVERIFY(QFileInfo::exists(protectedItem.path + ".kept"));
    }

    void rejectsRedirectedRecoveryJournalWithoutMovingSource() {
        QTemporaryDir temp(QDir::currentPath() + "/restore-journal-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        QTemporaryDir outside(QDir::currentPath() + "/restore-outside-XXXXXX.societycontainer"); QVERIFY(outside.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive); FileOperations ops(*drive);
        QFile source(temp.filePath("Files/keep.txt")); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("keep"); source.close();
        QVERIFY(createTestSymbolicLink(outside.path(), temp.filePath(".society-deleted")));
        const auto result = ops.perform(FileOperations::Action::Trash, source.fileName());
        QVERIFY(!result); QVERIFY(!result.error.isEmpty()); QVERIFY(QFileInfo::exists(source.fileName()));
        QVERIFY(!QFileInfo::exists(temp.filePath("Deleted/keep.txt")));
        QVERIFY(QDir(outside.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }

    void refusesPartiallyAvailableReplicaWithoutDownloading() {
        QTemporaryDir temp(QDir::currentPath() + "/restore-replica-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto created = SocietyDrive::create(temp.path()); QVERIFY(created);
        QVERIFY(SocietyDrive::adoptReplicaIdentity(temp.path(), created->identifier(), created->identifier()));
        QVERIFY(SocietyDrive::completeReplica(temp.path(), created->identifier()));
        const auto drive = SocietyDrive::open(temp.path()); QVERIFY(drive);
        StorageMap map(*drive); QVERIFY(!map.isLocalAuthority());
        QVERIFY(QDir().mkpath(temp.filePath("Deleted/package")));
        QFile present(temp.filePath("Deleted/package/present.bin")); QVERIFY(present.open(QIODevice::WriteOnly)); present.write("local"); present.close();
        QVERIFY(map.publish({QJsonObject{{"path", "deleted/package/present.bin"}, {"kind", "file"}, {"size", "5"}, {"resident", true}},
                             QJsonObject{{"path", "deleted/package/missing.bin"}, {"kind", "file"}, {"size", "1"}, {"resident", false}}}));
        FileOperations ops(*drive);
        const auto result = ops.perform(FileOperations::Action::RestoreTo, temp.filePath("Deleted/package"), temp.filePath("Files"));
        QVERIFY(!result); QVERIFY(result.error.contains("not fully available"));
        QVERIFY(QFileInfo::exists(present.fileName())); QVERIFY(!QFileInfo::exists(temp.filePath("Files/package")));
        QVERIFY(map.pendingRequests().isEmpty());
    }

    void ordinaryDirectoryWithoutDriveTerminates() {
        QTemporaryDir directory(QDir::currentPath() + "/ordinary-root-XXXXXX.societycontainer");
        QVERIFY(directory.isValid());
        QVERIFY(!FileOperations::containingDrive(directory.path()));
        QDir root(directory.path());
        while (!root.isRoot()) QVERIFY(root.cdUp());
        QVERIFY(!FileOperations::containingDrive(root.absolutePath()));
    }
    void temporaryLeaseMetadataIsProtectedAndNotCopied() {
        QTemporaryDir temp(QDir::currentPath() + "/metadata-filter-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive); FileOperations operations(*drive);
        QVERIFY(QDir().mkdir(temp.filePath("Files/folder")));
        QFile sentinel(temp.filePath("Files/folder/.iisacc-temporary.lock")); QVERIFY(sentinel.open(QIODevice::WriteOnly)); sentinel.close();
        QFile userLog(temp.filePath("Files/folder/user.log")); QVERIFY(userLog.open(QIODevice::WriteOnly)); userLog.write("user data"); userLog.close();
        QVERIFY(!operations.editable(sentinel.fileName()));
        QVERIFY(!operations.perform(FileOperations::Action::Trash, sentinel.fileName()));
        QVERIFY(!operations.perform(FileOperations::Action::Rename, sentinel.fileName(), "renamed"));
        const auto copied = operations.perform(FileOperations::Action::Duplicate, temp.filePath("Files/folder"));
        QVERIFY2(copied, qPrintable(copied.error));
        // The source lease is excluded by copyTree. Publishing the new file's
        // classification can independently create a fresh zero-byte lease.
        const QFileInfo copiedLease(copied.path + "/.iisacc-temporary.lock");
        if (copiedLease.exists()) { QVERIFY(copiedLease.isFile()); QVERIFY(!copiedLease.isSymLink()); QCOMPARE(copiedLease.size(), qint64(0)); }
        QVERIFY(QFileInfo::exists(copied.path + "/user.log"));
        QCOMPARE(QFileInfo(sentinel.fileName()).size(), qint64(0));
    }
    void createsDirectoriesAndMovesCompleteTrees() {
        QTemporaryDir temp(QDir::currentPath() + "/tree-manage-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        QTemporaryDir outside(QDir::currentPath() + "/tree-outside-XXXXXX.societycontainer"); QVERIFY(outside.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive); FileOperations ops(*drive);
        const auto project = ops.createDirectory(temp.filePath("Files"), QString::fromUtf8("자료 Project"));
        QVERIFY2(project, qPrintable(project.error));
        const auto sub = ops.createDirectory(project.path, "nested"); QVERIFY(sub);
        QFile file(sub.path + "/note.txt"); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("preserved"); file.close();
        QVERIFY(!ops.createDirectory(temp.filePath("Files"), QString::fromUtf8("자료 Project")));
        for (const auto& name : {"", ".", "..", "../escape", "a/b", "a\\b", "scheme:name", ".society-sync"})
            QVERIFY(!ops.createDirectory(project.path, name));
        QVERIFY(!ops.createDirectory(temp.path(), "protected"));
        QVERIFY(!ops.createDirectory(outside.path(), "escape"));
        QVERIFY(!ops.createDirectory(file.fileName(), "child"));
        QVERIFY(!ops.perform(FileOperations::Action::Move, project.path, sub.path));
        QVERIFY(!ops.perform(FileOperations::Action::Move, project.path, outside.path()));
        const auto destination = ops.createDirectory(temp.filePath("Files"), "archive"); QVERIFY(destination);
        const auto moved = ops.perform(FileOperations::Action::Move, project.path, destination.path);
        QVERIFY2(moved, qPrintable(moved.error)); QVERIFY(!QFileInfo::exists(project.path));
        QFile preserved(moved.path + "/nested/note.txt"); QVERIFY(preserved.open(QIODevice::ReadOnly));
        QCOMPARE(preserved.readAll(), QByteArray("preserved")); preserved.close();
        const auto other = ops.createDirectory(temp.filePath("Files"), QString::fromUtf8("자료 Project")); QVERIFY(other);
        QVERIFY(!ops.perform(FileOperations::Action::Move, other.path, destination.path));
        QVERIFY(QFileInfo::exists(other.path)); QVERIFY(QFileInfo::exists(moved.path + "/nested/note.txt"));
        QVERIFY(!ops.perform(FileOperations::Action::Move, temp.filePath("Models"), destination.path));
        const auto crossSection = ops.perform(FileOperations::Action::Move, moved.path, temp.filePath("Forked"));
        QVERIFY2(crossSection, qPrintable(crossSection.error));
        const auto updated = drive->tree("Forked"); QVERIFY2(updated, updated.error.message().c_str());
        QCOMPARE(updated.value->children.size(), std::size_t(1));
        const auto nativeRelative = updated.value->children.front().relativePath / "nested/note.txt";
        const auto* movedNode = updated.value->find(nativeRelative); QVERIFY(movedNode);
        QCOMPARE(movedNode->size, std::uintmax_t(9));
        const auto typed = drive->entry(QString::fromUtf8("Forked/자료 Project/nested/note.txt"));
        QVERIFY2(typed, typed.error.message().c_str());
        QVERIFY(updated.value->find(typed.value->relativePath));
        QVERIFY(QFile::link(outside.path(), temp.filePath("Files/link")));
        QVERIFY(!ops.createDirectory(temp.filePath("Files/link"), "escape"));
        QVERIFY(!ops.perform(FileOperations::Action::Move, crossSection.path, temp.filePath("Files/link")));
    }

    void deletionBypassesContentAndReplicationWork() {
        QTemporaryDir temp(QDir::currentPath() + "/direct-delete-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        StorageMap map(*drive); FileOperations ops(*drive);
        const auto source = temp.filePath("Models/Checkpoint/large.safetensors");
        constexpr qint64 bytes = 8LL * 1024 * 1024 * 1024;
        QVERIFY(QDir().mkdir(temp.filePath("Models/Checkpoint")));
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.resize(bytes)); file.close();
        // A stale map must neither download nor prevent moving the actual file.
        const QJsonObject entry{{"path", "models/Checkpoint/large.safetensors"}, {"kind", "file"},
            {"size", "1"}, {"resident", false}, {"version", QString(64, 'a')}};
        QVERIFY(map.publish({entry}));
        QLockFile sync(temp.filePath(".society-sync/operation.lock")); QVERIFY(sync.tryLock());
        QElapsedTimer elapsed; elapsed.start();
        const auto moved = ops.perform(FileOperations::Action::Trash, source);
        QVERIFY2(moved, qPrintable(moved.error));
        qInfo() << "8 GiB trash rename (ms):" << elapsed.elapsed();
        QVERIFY(!QFileInfo::exists(source)); QCOMPARE(QFileInfo(moved.path).size(), bytes);
        auto deleted = entry; deleted["path"] = "deleted/large.safetensors";
        QVERIFY(map.publish({entry, deleted})); elapsed.restart();
        const auto removed = ops.perform(FileOperations::Action::Remove, moved.path);
        QVERIFY2(removed, qPrintable(removed.error));
        qInfo() << "8 GiB permanent removal (ms):" << elapsed.elapsed();
        QVERIFY(!QFileInfo::exists(moved.path)); QVERIFY(map.pendingRequests().isEmpty());
        QCOMPARE(map.objects(), QJsonArray({entry, deleted})); // No graph/catalog mutation in the deletion path.
    }
    void removesPartialPackagesWithoutResolvingMissingChildren() {
        QTemporaryDir temp(QDir::currentPath() + "/partial-delete-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        QTemporaryDir outside(QDir::currentPath() + "/delete-keep-XXXXXX.societycontainer"); QVERIFY(outside.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive); StorageMap map(*drive);
        QVERIFY(QDir().mkpath(temp.filePath("Models/partial/sub")));
        QFile keep(outside.filePath("keep.txt")); QVERIFY(keep.open(QIODevice::WriteOnly)); keep.write("keep"); keep.close();
        QVERIFY(QFile::copy(keep.fileName(), temp.filePath("Models/partial/sub/present.bin")));
        QVERIFY(createTestSymbolicLink(outside.path(), temp.filePath("Models/partial/sub/link")));
        QVERIFY(map.publish({QJsonObject{{"path", "models/partial/missing.bin"}, {"kind", "file"}, {"resident", false}}}));
        FileOperations ops(*drive);
        const auto moved = ops.perform(FileOperations::Action::Trash, temp.filePath("Models/partial"));
        QVERIFY2(moved, qPrintable(moved.error));
        QVERIFY(QFileInfo::exists(moved.path + "/sub/present.bin"));
        QVERIFY(ops.perform(FileOperations::Action::Remove, moved.path));
        QVERIFY(!QFileInfo::exists(moved.path)); QVERIFY(QFileInfo::exists(keep.fileName()));
        QVERIFY(map.pendingRequests().isEmpty());
    }
    void handlesFilesPackagesAndCollisions() {
        QTemporaryDir temp(QDir::currentPath() + "/file-actions-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        FileOperations ops(*drive);
        const auto path = temp.filePath("Models/model.safetensors");
        QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("model bytes"); f.close();
        auto first = ops.perform(FileOperations::Action::Duplicate, path); QVERIFY2(first, qPrintable(first.error));
        auto second = ops.perform(FileOperations::Action::Duplicate, path); QVERIFY(second); QVERIFY(first.path != second.path);
        QFile copy(first.path); QVERIFY(copy.open(QIODevice::ReadOnly)); QCOMPARE(copy.readAll(), QByteArray("model bytes")); copy.close();
        auto renamed = ops.perform(FileOperations::Action::Rename, first.path, "renamed.safetensors"); QVERIFY(renamed); QVERIFY(!QFileInfo::exists(first.path));
        QVERIFY(!ops.perform(FileOperations::Action::Rename, renamed.path, "model.safetensors"));
        QVERIFY(QDir().mkpath(temp.filePath("Models/package/sub")));
        QVERIFY(QFile::copy(path, temp.filePath("Models/package/sub/weight.bin")));
        auto package = ops.perform(FileOperations::Action::Duplicate, temp.filePath("Models/package")); QVERIFY(package);
        QVERIFY(QFileInfo::exists(package.path + "/sub/weight.bin"));
        auto trashed = ops.perform(FileOperations::Action::Trash, temp.filePath("Models/package")); QVERIFY(trashed);
        QVERIFY(trashed.path.startsWith(temp.filePath("Deleted/"))); QVERIFY(QFileInfo::exists(trashed.path + "/sub/weight.bin"));
        auto restored = ops.perform(FileOperations::Action::Copy, trashed.path, temp.filePath("Models")); QVERIFY(restored);
        QVERIFY(ops.perform(FileOperations::Action::Remove, trashed.path)); QVERIFY(!QFileInfo::exists(trashed.path));
        QVERIFY(QFileInfo::exists(restored.path + "/sub/weight.bin"));
    }
    void protectsLayoutAndRejectsEscapes() {
        QTemporaryDir temp(QDir::currentPath() + "/file-action-boundary-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        QTemporaryDir outside(QDir::currentPath() + "/file-action-outside-XXXXXX.societycontainer"); QVERIFY(outside.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive); FileOperations ops(*drive);
        for (const auto &path : {temp.path(), temp.filePath("Models"), temp.filePath("Files")}) {
            QVERIFY(!ops.editable(path)); QVERIFY(!ops.perform(FileOperations::Action::Trash, path));
        }
        for (const auto *name : {"Documents", "Audios", "3D objects"}) {
            const auto path = temp.filePath("Files/" + QString(name));
            QVERIFY(QDir().mkdir(path)); QVERIFY(ops.editable(path));
            const auto renamed = ops.perform(FileOperations::Action::Rename, path, QString(name) + " renamed");
            QVERIFY2(renamed, qPrintable(renamed.error));
            QVERIFY(ops.perform(FileOperations::Action::Trash, renamed.path));
        }
        QVERIFY(QDir().mkdir(temp.filePath("Files/Documents")));
        QFile f(temp.filePath("Files/example.txt")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("keep"); f.close();
        for (const auto &name : {"../escape", ".society-hidden", "a/b", "", "."})
            QVERIFY(!ops.perform(FileOperations::Action::Rename, f.fileName(), name));
        QVERIFY(!ops.perform(FileOperations::Action::Copy, f.fileName(), outside.path()));
        QFile external(outside.filePath("from-finder.txt")); QVERIFY(external.open(QIODevice::WriteOnly)); external.write("external"); external.close();
        const auto pasted = ops.perform(FileOperations::Action::Copy, external.fileName(), temp.filePath("Files/Documents"));
        QVERIFY2(pasted, qPrintable(pasted.error)); QVERIFY(QFileInfo::exists(external.fileName()));
        QVERIFY(!ops.perform(FileOperations::Action::Remove, f.fileName())); // Only Deleted allows permanent removal.
        QVERIFY(createTestSymbolicLink(outside.path(), temp.filePath("Models/link")));
        QVERIFY(!ops.perform(FileOperations::Action::Trash, temp.filePath("Models/link")));
        QVERIFY(createTestSymbolicLink(outside.path(), temp.filePath("Deleted/link")));
        QVERIFY(!ops.perform(FileOperations::Action::Copy, f.fileName(), temp.filePath("Deleted/link")));
        QVERIFY(QFileInfo::exists(f.fileName())); QVERIFY(QFileInfo::exists(temp.filePath("Files/Documents")));
    }
};
QTEST_GUILESS_MAIN(FileOperationsTests)
#include "file_operations.moc"
