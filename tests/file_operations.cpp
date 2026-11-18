#include "SymbolicLink.h"
#include "FileOperations.h"
#include "StorageMap.h"
#include "SharedStorage.h"
#include "ModelStore.h"
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
#include "CTestSupport.h"
#include <QCoreApplication>
using namespace iiSocietyContainer;
namespace {
QString privateClassification(const SocietyDrive &drive, const QString &asset) {
    const auto logical = drive.relativePath(asset);
    for (const auto section : allStoreSections()) {
        const auto prefix = storeSectionName(section) + '/';
        if (!logical.startsWith(prefix)) continue;
        return drive.rootPath() + "/.society-runtime/classifications/" + storeSectionKey(section)
            + '/' + logical.mid(prefix.size()) + ".iisacc.json";
    }
    return {};
}
}
class FileOperationsTests : public QObject {
public:
    void classificationIsPrivateAcrossFileOperations() {
        QTemporaryDir temp(QDir::currentPath() + "/private-classification-XXXXXX.societycontainer");
        CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        const auto storage = SharedStorage::open(temp.path()); CTEST_CHECK(storage);
        const auto source = temp.filePath("Files/asset.txt");
        QFile file(source); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write("payload"), 7); file.close();
        QString error; CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::Files, "asset.txt", &error), qPrintable(error));
        const auto metadata = temp.filePath(".society-runtime/classifications/files/asset.txt.iisacc.json");
        CTEST_CHECK(QFileInfo::exists(metadata)); CTEST_CHECK(!QFileInfo::exists(source + ".iisacc.json"));
        const auto identity = storage->fileTracking(StoreSection::Files, "asset.txt").value("objectKey");
        CTEST_CHECK(!identity.toString().isEmpty());
        FileOperations operations(*drive);
        const auto renamed = operations.perform(FileOperations::Action::Rename, source, "renamed.txt");
        CTEST_CHECK_MESSAGE(renamed, qPrintable(renamed.error));
        CTEST_CHECK(!QFileInfo::exists(renamed.path + ".iisacc.json"));
        CTEST_CHECK(QFileInfo::exists(temp.filePath(".society-runtime/classifications/files/renamed.txt.iisacc.json")));
        CTEST_COMPARE(storage->fileTracking(StoreSection::Files, "renamed.txt").value("objectKey"), identity);
        const auto trashed = operations.perform(FileOperations::Action::Trash, renamed.path);
        CTEST_CHECK_MESSAGE(trashed, qPrintable(trashed.error));
        CTEST_CHECK(!QFileInfo::exists(trashed.path + ".iisacc.json"));
        CTEST_CHECK(QFileInfo::exists(temp.filePath(".society-runtime/classifications/deleted/renamed.txt.iisacc.json")));
        const auto restored = operations.perform(FileOperations::Action::Restore, trashed.path);
        CTEST_CHECK_MESSAGE(restored, qPrintable(restored.error));
        CTEST_COMPARE(storage->fileTracking(StoreSection::Files, "renamed.txt").value("objectKey"), identity);
        const auto copy = operations.perform(FileOperations::Action::Duplicate, restored.path);
        CTEST_CHECK_MESSAGE(copy, qPrintable(copy.error));
        CTEST_CHECK(!QFileInfo::exists(copy.path + ".iisacc.json"));
        CTEST_CHECK(storage->fileTracking(StoreSection::Files, QFileInfo(copy.path).fileName()).value("objectKey") != identity);
    }
    void modelCategoryKeepsPrivateClassification() {
        QTemporaryDir temp(QDir::currentPath() + "/private-model-XXXXXX.societycontainer");
        CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        const auto source = temp.filePath("Models/Checkpoint/model.safetensors");
        CTEST_CHECK(QDir().mkpath(QFileInfo(source).absolutePath()));
        QFile file(source); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("model"); file.close();
        const auto base = std::filesystem::path(temp.filePath("Models").toStdU16String());
        const auto vault = FileRepository::classificationDirectory(std::filesystem::path(temp.path().toStdU16String()), "models");
        iiFileProvider::FileCatalog catalog(base, iiFileProvider::FileCatalog::Access::Create, vault);
        iiFileProvider::FileMetadata metadata; metadata.title = "Model classification";
        const auto original = catalog.registerFile("Checkpoint/model.safetensors", metadata);
        CTEST_CHECK(original.metadataWritten);
        QString error; const auto store = ModelStore::open(temp.path(), &error); CTEST_CHECK_MESSAGE(store, qPrintable(error));
        const auto moved = store->place(source, ModelType::LLM, &error); CTEST_CHECK_MESSAGE(!moved.isEmpty(), qPrintable(error));
        CTEST_CHECK(!QFileInfo::exists(moved + ".iisacc.json"));
        CTEST_CHECK(QFileInfo::exists(temp.filePath(".society-runtime/classifications/models/LLM/model.safetensors.iisacc.json")));
        CTEST_COMPARE(store->indexAsset(moved, &error), QString::fromStdString(original.file.key));
        const auto current = catalog.lookup(original.file.key); CTEST_CHECK(current);
        CTEST_COMPARE(QString::fromStdString(current->metadata.title), QStringLiteral("Model classification"));
    }
    void movesProjectPackagesAsSingleItems() {
        QTemporaryDir temp(QDir::currentPath() + "/project-package-move-XXXXXX.societycontainer");
        CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        CTEST_CHECK(QDir().mkpath(temp.filePath("Files/Example.iiproject")));
        QFile manifest(temp.filePath("Files/Example.iiproject/manifest.json"));
        CTEST_CHECK(manifest.open(QIODevice::WriteOnly)); CTEST_COMPARE(manifest.write("{}"), 2); manifest.close();
        CTEST_CHECK(!FileOperations(*drive).perform(FileOperations::Action::Duplicate, temp.filePath("Files/Example.iiproject")));
        CTEST_CHECK(!QFileInfo::exists(temp.filePath("Files/Example copy.iiproject")));
        const auto renamed = FileOperations(*drive).perform(FileOperations::Action::Rename,
            temp.filePath("Files/Example.iiproject"), "Renamed.iiproject");
        CTEST_CHECK_MESSAGE(renamed, qPrintable(renamed.error));
        CTEST_CHECK(QFileInfo::exists(renamed.path + "/manifest.json"));
        CTEST_CHECK(!QFileInfo::exists(temp.filePath("Files/Example.iiproject")));
        const auto moved = FileOperations(*drive).perform(FileOperations::Action::Move, renamed.path, temp.filePath("Forked"));
        CTEST_CHECK_MESSAGE(moved, qPrintable(moved.error));
        CTEST_CHECK(QFileInfo::exists(moved.path + "/manifest.json"));
        QFile forged(moved.path + ".iisacc.json"); CTEST_CHECK(forged.open(QIODevice::WriteOnly)); forged.write("{}"); forged.close();
        const auto rejected = FileOperations(*drive).perform(FileOperations::Action::Rename, moved.path, "Other.iiproject");
        CTEST_CHECK(!rejected);
        CTEST_CHECK(QFileInfo::exists(moved.path + "/manifest.json"));
    }
    void relocationParticipantRunsWithinTheMove() {
        QTemporaryDir temp(QDir::currentPath() + "/move-participant-XXXXXX.societycontainer");
        CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        QFile file(temp.filePath("Files/original.txt")); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("source"); file.close();
        QList<FileOperations::RelocationPhase> phases;
        FileOperations::setRelocationHandler([&](const QString &, const QString &source, const QString &destination,
                                                  FileOperations::RelocationPhase phase) {
            phases.append(phase);
            if (phase == FileOperations::RelocationPhase::Prepare) {
                CTEST_CHECK(QFileInfo::exists(source)); CTEST_CHECK(!QFileInfo::exists(destination));
            }
            if (phase == FileOperations::RelocationPhase::Commit) {
                CTEST_CHECK(!QFileInfo::exists(source)); CTEST_CHECK(QFileInfo::exists(destination));
            }
        });
        const auto result = FileOperations(*drive).perform(FileOperations::Action::Rename, file.fileName(), "renamed.txt");
        FileOperations::setRelocationHandler({});
        CTEST_CHECK_MESSAGE(result, qPrintable(result.error));
        CTEST_COMPARE(phases, (QList<FileOperations::RelocationPhase>{FileOperations::RelocationPhase::Prepare, FileOperations::RelocationPhase::Commit}));
    }
    void caseOnlyRenameKeepsTheItem() {
        QTemporaryDir temp(QDir::currentPath() + "/case-only-move-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        QFile file(temp.filePath("Files/lower.txt")); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("content"); file.close();
        if (!QFileInfo::exists(temp.filePath("Files/LOWER.txt"))) society_ctest::skip("The test filesystem distinguishes case.");
        const auto result = FileOperations(*drive).perform(FileOperations::Action::Rename, file.fileName(), "LOWER.txt");
        CTEST_CHECK_MESSAGE(result, qPrintable(result.error));
        CTEST_COMPARE(QFileInfo(result.path).fileName(), QStringLiteral("LOWER.txt"));
        QFile output(result.path); CTEST_CHECK(output.open(QIODevice::ReadOnly)); CTEST_COMPARE(output.readAll(), QByteArray("content"));
    }
    void relocationFailureReportsWhichSideMoved() {
        QTemporaryDir temp(QDir::currentPath() + "/move-failures-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        QFile file(temp.filePath("Files/original.txt")); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("content"); file.close();
        FileOperations::setRelocationHandler([](const QString &, const QString &, const QString &,
                                                FileOperations::RelocationPhase phase) {
            if (phase == FileOperations::RelocationPhase::Prepare) throw std::runtime_error("prepare denied");
        });
        const auto denied = FileOperations(*drive).perform(FileOperations::Action::Rename, file.fileName(), "denied.txt");
        FileOperations::setRelocationHandler({});
        CTEST_CHECK(!denied); CTEST_CHECK(denied.path.isEmpty()); CTEST_CHECK(QFileInfo::exists(file.fileName()));
        CTEST_CHECK(!QFileInfo::exists(temp.filePath("Files/denied.txt")));
        FileOperations::setRelocationHandler([](const QString &, const QString &, const QString &,
                                                FileOperations::RelocationPhase phase) {
            if (phase == FileOperations::RelocationPhase::Commit) throw std::runtime_error("commit interrupted");
        });
        const auto partial = FileOperations(*drive).perform(FileOperations::Action::Rename, file.fileName(), "moved.txt");
        FileOperations::setRelocationHandler({});
        CTEST_CHECK(!partial); CTEST_COMPARE(partial.path, temp.filePath("Files/moved.txt"));
        CTEST_CHECK(QFileInfo::exists(partial.path)); CTEST_CHECK(!QFileInfo::exists(file.fileName()));
    }

    void deletedLocksSeparateIndependentAssetsAndProtectRelatedPaths() {
        QTemporaryDir temp(QDir::currentPath() + "/deleted-lock-scope-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        detail::DeletedOrigins first(*drive), independent(*drive), child(*drive), model(*drive), companion(*drive);
        QString error;
        CTEST_CHECK_MESSAGE(first.lockPaths({temp.filePath("Deleted/package")}, &error, 0), qPrintable(error));
        CTEST_CHECK_MESSAGE(independent.lockPaths({temp.filePath("Deleted/other")}, &error, 0), qPrintable(error));
        CTEST_CHECK(!child.lockPaths({temp.filePath("Deleted/package/nested.bin")}, &error, 0));
        CTEST_CHECK_MESSAGE(model.lockPaths({temp.filePath("Deleted/example.safetensors")}, &error, 0), qPrintable(error));
        CTEST_CHECK(!companion.lockPaths({temp.filePath("Deleted/example.preview.png")}, &error, 0));
        first.unlock();
        CTEST_CHECK_MESSAGE(child.lockPaths({temp.filePath("Deleted/package/nested.bin")}, &error, 0), qPrintable(error));
    }
    void providerControlsCannotBeManipulatedAsFiles() {
        QTemporaryDir temp(QDir::currentPath() + "/protected-metadata-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        const auto files = temp.filePath("Files");
        QFile asset(files + "/document.json"); CTEST_CHECK(asset.open(QIODevice::WriteOnly));
        CTEST_COMPARE(asset.write("{}"), 2); asset.close();
        iiFileProvider::FileCatalog catalog(std::filesystem::path(files.toStdU16String()));
        CTEST_CHECK(catalog.registerFile("document.json").metadataWritten);
        const auto metadata = files + "/document.json.iisacc.json";
        CTEST_CHECK(QFileInfo::exists(metadata));
        FileOperations operations(*drive);
        CTEST_CHECK(!operations.editable(metadata));
        CTEST_CHECK(!operations.perform(FileOperations::Action::Rename, metadata, "exposed.json"));
        CTEST_CHECK(!operations.perform(FileOperations::Action::Trash, metadata));
        CTEST_CHECK(!operations.perform(FileOperations::Action::Copy, metadata, files));
        CTEST_CHECK(!operations.perform(FileOperations::Action::Rename, asset.fileName(), "fake.iisacc.json"));
        CTEST_CHECK(!QFileInfo::exists(metadata));
        CTEST_CHECK(QFileInfo::exists(privateClassification(*drive, asset.fileName())));
        CTEST_CHECK(!operations.editable(privateClassification(*drive, asset.fileName())));
        CTEST_CHECK(QFileInfo::exists(asset.fileName()));
        CTEST_CHECK(operations.editable(asset.fileName()));
    }
    void orphanedDestinationMetadataIsNeverAdopted() {
        QTemporaryDir temp(QDir::currentPath() + "/orphan-metadata-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        QTemporaryDir source(QDir::currentPath() + "/orphan-source-XXXXXX"); CTEST_CHECK(source.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        const auto input = source.filePath("asset.txt");
        QFile asset(input); CTEST_CHECK(asset.open(QIODevice::WriteOnly)); CTEST_COMPARE(asset.write("source"), 6); asset.close();
        const auto folder = temp.filePath("Files");
        const auto orphan = folder + "/asset.txt.iisacc.json";
        QFile foreign(orphan); CTEST_CHECK(foreign.open(QIODevice::WriteOnly)); CTEST_COMPARE(foreign.write("unrelated"), 9); foreign.close();
        const auto copied = FileOperations(*drive).perform(FileOperations::Action::Copy, input, folder);
        CTEST_CHECK_MESSAGE(copied, qPrintable(copied.error));
        CTEST_COMPARE(copied.path, folder + "/asset 2.txt");
        CTEST_CHECK(!FileOperations(*drive).perform(FileOperations::Action::Rename, copied.path, "asset.txt"));
        CTEST_CHECK(foreign.open(QIODevice::ReadOnly)); CTEST_COMPARE(foreign.readAll(), QByteArray("unrelated"));
        CTEST_CHECK(QFileInfo::exists(copied.path));
    }
    void stagesAndRecoversCompleteAssetGroupsAcrossVolumes() {
        QTemporaryDir temp(QDir::currentPath() + "/asset-transfer-XXXXXX"); CTEST_CHECK(temp.isValid());
        CTEST_CHECK(QDir().mkdir(temp.filePath("from"))); CTEST_CHECK(QDir().mkdir(temp.filePath("to")));
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
        for (qsizetype i = 0; i < moves.size(); ++i) CTEST_CHECK(write(moves[i].first, bytes[i]));
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
        CTEST_CHECK(!result.committed); CTEST_CHECK(result.originalsRestored); CTEST_CHECK(result.error.contains("changed"));
        CTEST_COMPARE(read(source), QByteArray("model!"));
        CTEST_CHECK(!QFileInfo::exists(target)); CTEST_CHECK(write(source, bytes[0]));
        CTEST_CHECK(write(target + ".iisacc.json", "other metadata"));
        result = detail::transferAssetGroup(moves, copy, move, remove, [](QString *) { return true; });
        CTEST_CHECK(!result.committed); CTEST_CHECK(result.originalsRestored);
        for (qsizetype i = 0; i < moves.size(); ++i) CTEST_COMPARE(read(moves[i].first), bytes[i]);
        CTEST_COMPARE(read(target + ".iisacc.json"), QByteArray("other metadata"));
        CTEST_CHECK(QFile::remove(target + ".iisacc.json"));
        result = detail::transferAssetGroup(moves, copy, move, remove, [](QString *) { return true; });
        CTEST_CHECK_MESSAGE(result.committed, qPrintable(result.error)); CTEST_CHECK(result.error.isEmpty());
        for (qsizetype i = 0; i < moves.size(); ++i) {
            CTEST_CHECK(!QFileInfo::exists(moves[i].first)); CTEST_COMPARE(read(moves[i].second), bytes[i]);
        }
        CTEST_CHECK(QDir(temp.filePath("from")).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());

        // Private classification files live in separate directories from the
        // public payload, including when the public volume needs a transfer.
        CTEST_CHECK(QDir().mkdir(temp.filePath("private-from")));
        CTEST_CHECK(QDir().mkdir(temp.filePath("private-to")));
        const detail::AssetMoveList split{{temp.filePath("from/asset.txt"), temp.filePath("to/asset.txt")},
            {temp.filePath("private-from/asset.txt.iisacc.json"), temp.filePath("private-to/asset.txt.iisacc.json")}};
        CTEST_CHECK(write(split[0].first, "data")); CTEST_CHECK(write(split[1].first, "classification"));
        const auto splitResult = detail::transferAssetGroup(split, copy, move, remove, [](QString *) { return true; });
        CTEST_CHECK_MESSAGE(splitResult.committed, qPrintable(splitResult.error));
        CTEST_COMPARE(read(split[0].second), QByteArray("data")); CTEST_COMPARE(read(split[1].second), QByteArray("classification"));
        CTEST_CHECK(QFile::remove(split[0].second)); CTEST_CHECK(QFile::remove(split[1].second));

        // A failed journal commit preserves both published copies and originals.
        for (qsizetype i = 0; i < moves.size(); ++i) {
            CTEST_CHECK(QFile::remove(moves[i].second)); CTEST_CHECK(write(moves[i].first, bytes[i]));
        }
        result = detail::transferAssetGroup(moves, copy, move, remove, [](QString *error) {
            *error = QStringLiteral("journal failed"); return false;
        });
        CTEST_CHECK(!result.committed); CTEST_CHECK(result.originalsRestored); CTEST_CHECK(result.error.contains("journal failed"));
        for (qsizetype i = 0; i < moves.size(); ++i) {
            CTEST_COMPARE(read(moves[i].first), bytes[i]); CTEST_COMPARE(read(moves[i].second), bytes[i]);
        }
        for (const auto &item : moves) CTEST_CHECK(QFile::remove(item.second));
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
        CTEST_CHECK(result.committed); CTEST_CHECK(result.error.contains("changed")); CTEST_CHECK(!changedRecovery.isEmpty());
        CTEST_COMPARE(read(target), bytes[0]); CTEST_COMPARE(read(changedRecovery), QByteArray("model!"));
        CTEST_CHECK(QFileInfo::exists(QFileInfo(changedRecovery).absolutePath() + "/model.safetensors.iisacc.json"));
    }

    void movesAndRestoresFileClassificationWithTheAsset() {
        QTemporaryDir temp(QDir::currentPath() + "/file-object-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        const auto source = temp.filePath("Files/document.txt");
        QFile file(source); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_CHECK(file.write("asset bytes") > 0); file.close();
        iiFileProvider::FileCatalog catalog(std::filesystem::path(temp.filePath("Files").toStdU16String()));
        iiFileProvider::FileMetadata metadata; metadata.title = "Important asset";
        CTEST_CHECK(catalog.registerFile("document.txt", metadata).metadataWritten);
        const auto originalCatalog = catalog.lookupPath("document.txt"); CTEST_CHECK(originalCatalog);
        const auto originalProviderKey = originalCatalog->key;
        const auto preview = [](const QString &asset) {
            const QFileInfo info(asset); return info.absolutePath() + "/." + info.fileName() + ".iisacc-preview";
        };
        CTEST_CHECK(QFileInfo::exists(preview(source)));
        const auto storage = SharedStorage::open(temp.path()); CTEST_CHECK(storage);
        QString error; CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::Files, "document.txt", &error), qPrintable(error));
        const auto before = storage->fileTracking(StoreSection::Files, "document.txt").value("objectKey");
        CTEST_CHECK(!before.toString().isEmpty());
        CTEST_CHECK(QDir().mkpath(temp.filePath("Files/archive")));
        FileOperations operations(*drive);
        const auto moved = operations.perform(FileOperations::Action::Move, source, temp.filePath("Files/archive"));
        CTEST_CHECK_MESSAGE(moved, qPrintable(moved.error));
        CTEST_CHECK(!QFileInfo::exists(source + ".iisacc.json"));
        CTEST_CHECK(!QFileInfo::exists(moved.path + ".iisacc.json"));
        CTEST_CHECK(QFileInfo::exists(privateClassification(*drive, moved.path)));
        CTEST_CHECK(!QFileInfo::exists(preview(source))); CTEST_CHECK(QFileInfo::exists(preview(moved.path)));
        CTEST_COMPARE(storage->fileTracking(StoreSection::Files, "archive/document.txt").value("objectKey"), before);
        const auto trashed = operations.perform(FileOperations::Action::Trash, moved.path);
        CTEST_CHECK_MESSAGE(trashed, qPrintable(trashed.error));
        CTEST_CHECK(!QFileInfo::exists(moved.path + ".iisacc.json"));
        CTEST_CHECK(!QFileInfo::exists(trashed.path + ".iisacc.json"));
        CTEST_CHECK(QFileInfo::exists(privateClassification(*drive, trashed.path)));
        CTEST_CHECK(!QFileInfo::exists(preview(moved.path))); CTEST_CHECK(QFileInfo::exists(preview(trashed.path)));
        const auto restored = operations.perform(FileOperations::Action::Restore, trashed.path);
        CTEST_CHECK_MESSAGE(restored, qPrintable(restored.error)); CTEST_COMPARE(restored.path, moved.path);
        CTEST_CHECK(!QFileInfo::exists(trashed.path + ".iisacc.json"));
        CTEST_CHECK(!QFileInfo::exists(restored.path + ".iisacc.json"));
        CTEST_CHECK(QFileInfo::exists(privateClassification(*drive, restored.path)));
        CTEST_CHECK(!QFileInfo::exists(preview(trashed.path))); CTEST_CHECK(QFileInfo::exists(preview(restored.path)));
        CTEST_COMPARE(storage->fileTracking(StoreSection::Files, "archive/document.txt").value("objectKey"), before);
        const auto duplicate = operations.perform(FileOperations::Action::Duplicate, restored.path);
        CTEST_CHECK_MESSAGE(duplicate, qPrintable(duplicate.error));
        CTEST_CHECK(QFileInfo::exists(privateClassification(*drive, restored.path)));
        CTEST_CHECK(QFileInfo::exists(privateClassification(*drive, duplicate.path)));
        CTEST_CHECK(QFileInfo::exists(preview(restored.path))); CTEST_CHECK(QFileInfo::exists(preview(duplicate.path)));
        const auto duplicateRelative = QDir(temp.filePath("Files")).relativeFilePath(duplicate.path);
        const auto duplicateRecord = storage->fileTracking(StoreSection::Files, duplicateRelative);
        CTEST_CHECK(!duplicateRecord.value("objectKey").toString().isEmpty());
        CTEST_CHECK(duplicateRecord.value("objectKey") != before);
        const auto duplicateCatalog = catalog.lookupPath(duplicateRelative.toStdString());
        CTEST_CHECK(duplicateCatalog); CTEST_COMPARE(QString::fromStdString(duplicateCatalog->metadata.title), QStringLiteral("Important asset"));
        CTEST_CHECK(duplicateCatalog->key != originalProviderKey);
        QFile duplicateMetadata(privateClassification(*drive, duplicate.path)); CTEST_CHECK(duplicateMetadata.open(QIODevice::ReadOnly));
        CTEST_COMPARE(QJsonDocument::fromJson(duplicateMetadata.readAll()).object().value("fileKey").toString(),
                 QString::fromStdString(duplicateCatalog->key));
        duplicateMetadata.close();
        const auto duplicateTrash = operations.perform(FileOperations::Action::Trash, duplicate.path);
        CTEST_CHECK_MESSAGE(duplicateTrash, qPrintable(duplicateTrash.error));
        CTEST_CHECK(QFileInfo::exists(privateClassification(*drive, duplicateTrash.path)));
        const auto removed = operations.perform(FileOperations::Action::Remove, duplicateTrash.path);
        CTEST_CHECK_MESSAGE(removed, qPrintable(removed.error));
        CTEST_CHECK(!QFileInfo::exists(duplicateTrash.path));
        CTEST_CHECK(!QFileInfo::exists(privateClassification(*drive, duplicateTrash.path)));
        CTEST_CHECK(!QFileInfo::exists(preview(duplicateTrash.path)));
        CTEST_CHECK(QFileInfo::exists(restored.path)); CTEST_CHECK(QFileInfo::exists(privateClassification(*drive, restored.path)));
    }

    void duplicatesModelAndItsAssociatedAssetWithDistinctIdentity() {
        QTemporaryDir temp(QDir::currentPath() + "/model-object-copy-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        const auto model = temp.filePath("Models/Other/model.safetensors");
        const auto preview = temp.filePath("Models/Other/model.preview.png");
        CTEST_CHECK(QDir().mkpath(QFileInfo(model).absolutePath()));
        for (const auto &path : {model, preview}) {
            QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_CHECK(file.write("asset") > 0); file.close();
        }
        iiFileProvider::FileCatalog catalog(std::filesystem::path(temp.filePath("Models").toStdU16String()));
        iiFileProvider::FileMetadata metadata; metadata.title = "Model preview";
        CTEST_CHECK(catalog.registerFile("Other/model.safetensors").metadataWritten);
        CTEST_CHECK(catalog.registerFile("Other/model.preview.png", metadata).metadataWritten);
        const auto storage = SharedStorage::open(temp.path()); CTEST_CHECK(storage); QString error;
        CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::Models, "Other/model.safetensors", &error), qPrintable(error));
        CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::Models, "Other/model.preview.png", &error), qPrintable(error));
        const auto original = storage->fileTracking(StoreSection::Models, "Other/model.preview.png").value("resource").toObject().value("id");
        CTEST_CHECK(!original.toString().isEmpty());
        const auto result = FileOperations(*drive).perform(FileOperations::Action::Duplicate, model);
        CTEST_CHECK_MESSAGE(result, qPrintable(result.error));
        const auto copiedPreview = temp.filePath("Models/Other/model copy.preview.png");
        CTEST_CHECK(QFileInfo::exists(copiedPreview)); CTEST_CHECK(QFileInfo::exists(privateClassification(*drive, copiedPreview)));
        const auto copiedTracking = storage->fileTracking(StoreSection::Models, "Other/model copy.preview.png");
        CTEST_COMPARE(copiedTracking.value("resourceStatus").toString(), QStringLiteral("working-copy-only"));
        CTEST_CHECK(copiedTracking.value("objectKey").toString().isEmpty());
        const auto copied = copiedTracking.value("resource").toObject().value("id");
        CTEST_CHECK(!copied.toString().isEmpty()); CTEST_CHECK(copied != original);
        const auto copiedCatalog = catalog.lookupPath("Other/model copy.preview.png"); CTEST_CHECK(copiedCatalog);
        CTEST_COMPARE(QString::fromStdString(copiedCatalog->metadata.title), QStringLiteral("Model preview"));
        QFile copiedMetadata(privateClassification(*drive, copiedPreview)); CTEST_CHECK(copiedMetadata.open(QIODevice::ReadOnly));
        CTEST_COMPARE(QJsonDocument::fromJson(copiedMetadata.readAll()).object().value("fileKey").toString(),
                 QString::fromStdString(copiedCatalog->key));
        copiedMetadata.close();
        const auto trashed = FileOperations(*drive).perform(FileOperations::Action::Trash, result.path);
        CTEST_CHECK_MESSAGE(trashed, qPrintable(trashed.error));
        const auto deletedPreview = QFileInfo(trashed.path).absolutePath() + "/model copy.preview.png";
        CTEST_CHECK(QFileInfo::exists(deletedPreview));
        const auto restored = FileOperations(*drive).perform(FileOperations::Action::Restore, trashed.path);
        CTEST_CHECK_MESSAGE(restored, qPrintable(restored.error)); CTEST_COMPARE(restored.path, result.path);
        CTEST_CHECK(QFileInfo::exists(copiedPreview)); CTEST_CHECK(QFileInfo::exists(privateClassification(*drive, copiedPreview)));
        CTEST_CHECK(!QFileInfo::exists(deletedPreview));
    }

    void importingModelCopiesRecognizedCompanions() {
        QTemporaryDir temp(QDir::currentPath() + "/model-object-import-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        QTemporaryDir external(QDir::currentPath() + "/model-external-XXXXXX"); CTEST_CHECK(external.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        const auto model = external.filePath("model.safetensors");
        const auto description = external.filePath("model.model.json");
        for (const auto &path : {model, description}) {
            QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_CHECK(file.write("asset") > 0); file.close();
        }
        iiFileProvider::FileCatalog externalCatalog(std::filesystem::path(external.path().toStdU16String()));
        iiFileProvider::FileMetadata metadata; metadata.title = "Imported description";
        CTEST_CHECK(externalCatalog.registerFile("model.safetensors").metadataWritten);
        CTEST_CHECK(externalCatalog.registerFile("model.model.json", metadata).metadataWritten);
        const auto folder = temp.filePath("Models/Other"); CTEST_CHECK(QDir().mkpath(folder));
        const auto storage = SharedStorage::open(temp.path()); CTEST_CHECK(storage);
        auto repository = storage->fileRepository();
        const auto identity = drive->storageKind() == SocietyDrive::StorageKind::LogicalDisk
            ? drive->storageIdentifier() : drive->identifier();
        const auto package = FileRepository::packagePath(std::filesystem::path(temp.path().toStdU16String()), identity.toStdString());
        iiFileProvider::TemporaryLease lock(package / "operations", iiFileProvider::TemporaryLease::Mode::TryExclusive);
        CTEST_CHECK(lock.locked());
        const auto result = FileOperations(*drive).perform(FileOperations::Action::Copy, model, folder);
        CTEST_CHECK_MESSAGE(result, qPrintable(result.error));
        CTEST_CHECK(lock.locked());
        const auto copiedDescription = folder + "/model.model.json";
        CTEST_CHECK(!QFileInfo::exists(result.path + ".iisacc.json"));
        CTEST_CHECK(QFileInfo::exists(privateClassification(*drive, result.path)));
        CTEST_CHECK(QFileInfo::exists(copiedDescription)); CTEST_CHECK(QFileInfo::exists(privateClassification(*drive, copiedDescription)));
        CTEST_CHECK(QFileInfo::exists(model)); CTEST_CHECK(QFileInfo::exists(description));
        const auto tracking = storage->fileTracking(StoreSection::Models, "Other/model.model.json");
        CTEST_COMPARE(tracking.value("resourceStatus").toString(), QStringLiteral("working-copy-only"));
        CTEST_CHECK(tracking.value("objectKey").toString().isEmpty());
        CTEST_CHECK(!tracking.value("resource").toObject().value("id").toString().isEmpty());
        CTEST_CHECK(!repository->lookup("models/Other/model.safetensors"));
        CTEST_CHECK(!repository->lookup("models/Other/model.model.json"));
        iiFileProvider::FileCatalog destinationCatalog(std::filesystem::path(temp.filePath("Models").toStdU16String()));
        const auto copied = destinationCatalog.lookupPath("Other/model.model.json"); CTEST_CHECK(copied);
        CTEST_COMPARE(QString::fromStdString(copied->metadata.title), QStringLiteral("Imported description"));
    }

    void nativeRenameNeverReplacesAnExistingItem() {
        QTemporaryDir temp(QDir::currentPath() + "/exclusive-rename-XXXXXX"); CTEST_CHECK(temp.isValid());
        const auto source = temp.filePath("source.bin");
        const auto target = temp.filePath("target.bin");
        QFile first(source), second(target);
        CTEST_CHECK(first.open(QIODevice::WriteOnly)); CTEST_COMPARE(first.write("source"), 6); first.close();
        CTEST_CHECK(second.open(QIODevice::WriteOnly)); CTEST_COMPARE(second.write("target"), 6); second.close();
        CTEST_CHECK(!detail::renameWithoutCopy(source, target));
        CTEST_CHECK(first.open(QIODevice::ReadOnly)); CTEST_COMPARE(first.readAll(), QByteArray("source")); first.close();
        CTEST_CHECK(second.open(QIODevice::ReadOnly)); CTEST_COMPARE(second.readAll(), QByteArray("target")); second.close();
        CTEST_CHECK(QFile::remove(target));
        CTEST_CHECK(detail::renameWithoutCopy(source, target));
        CTEST_CHECK(!QFileInfo::exists(source));
        CTEST_CHECK(second.open(QIODevice::ReadOnly)); CTEST_COMPARE(second.readAll(), QByteArray("source"));
    }
    void restoresDeletedItemsWithoutOverwritingAndKeepsOrigins() {
        QTemporaryDir temp(QDir::currentPath() + "/restore-deleted-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive); FileOperations ops(*drive);
        CTEST_CHECK(QDir().mkpath(temp.filePath("Files/projects")));
        const auto original = temp.filePath("Files/projects/example.txt");
        QFile source(original); CTEST_CHECK(source.open(QIODevice::WriteOnly)); source.write("original"); source.close();
        const auto trashed = ops.perform(FileOperations::Action::Trash, original);
        CTEST_CHECK_MESSAGE(trashed, qPrintable(trashed.error)); CTEST_CHECK(!QFileInfo::exists(original));
        QFile replacement(original); CTEST_CHECK(replacement.open(QIODevice::WriteOnly)); replacement.write("replacement"); replacement.close();
        const auto restored = ops.perform(FileOperations::Action::Restore, trashed.path);
        CTEST_CHECK_MESSAGE(restored, qPrintable(restored.error));
        CTEST_CHECK(restored.path != original); CTEST_CHECK(!QFileInfo::exists(trashed.path));
        QFile restoredFile(restored.path); CTEST_CHECK(restoredFile.open(QIODevice::ReadOnly)); CTEST_COMPARE(restoredFile.readAll(), QByteArray("original"));
        CTEST_CHECK(replacement.open(QIODevice::ReadOnly)); CTEST_COMPARE(replacement.readAll(), QByteArray("replacement"));

        CTEST_CHECK(QDir().mkpath(temp.filePath("Models/package/sub")));
        QFile model(temp.filePath("Models/package/sub/model.bin")); CTEST_CHECK(model.open(QIODevice::WriteOnly)); model.write("weights"); model.close();
        const auto package = ops.perform(FileOperations::Action::Trash, temp.filePath("Models/package")); CTEST_CHECK(package);
        const auto child = ops.perform(FileOperations::Action::Restore, package.path + "/sub/model.bin");
        CTEST_CHECK_MESSAGE(child, qPrintable(child.error)); CTEST_COMPARE(child.path, temp.filePath("Models/package/sub/model.bin"));
        CTEST_CHECK(QFileInfo::exists(child.path));

        QFile unknown(temp.filePath("Deleted/unknown.txt")); CTEST_CHECK(unknown.open(QIODevice::WriteOnly)); unknown.write("keep"); unknown.close();
        const auto noOrigin = ops.perform(FileOperations::Action::Restore, unknown.fileName());
        CTEST_CHECK(!noOrigin); CTEST_CHECK(!noOrigin.error.isEmpty()); CTEST_CHECK(QFileInfo::exists(unknown.fileName()));
        CTEST_CHECK(!ops.perform(FileOperations::Action::RestoreTo, unknown.fileName(), temp.filePath("Deleted")));
        CTEST_CHECK(!ops.perform(FileOperations::Action::RestoreTo, unknown.fileName(), QFileInfo(temp.path()).absolutePath()));
        QFile existing(temp.filePath("Files/projects/unknown.txt")); CTEST_CHECK(existing.open(QIODevice::WriteOnly)); existing.write("existing"); existing.close();
        const auto chosen = ops.perform(FileOperations::Action::RestoreTo, unknown.fileName(), temp.filePath("Files/projects"));
        CTEST_CHECK_MESSAGE(chosen, qPrintable(chosen.error)); CTEST_CHECK(chosen.path != existing.fileName());
        CTEST_CHECK(QFileInfo::exists(chosen.path)); CTEST_CHECK(QFileInfo::exists(existing.fileName()));
        CTEST_CHECK(!ops.perform(FileOperations::Action::Restore, original));
    }

    void renamedAndLegacyDeletedItemsKeepTrustworthyOrigins() {
        QTemporaryDir temp(QDir::currentPath() + "/restore-origin-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive); FileOperations ops(*drive);
        const auto storage = SharedStorage::open(temp.path()); CTEST_CHECK(storage);
        QFile legacy(temp.filePath("Files/legacy.txt")); CTEST_CHECK(legacy.open(QIODevice::WriteOnly)); legacy.write("legacy"); legacy.close();
        QString error; CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::Files, "legacy.txt", &error), qPrintable(error));
        const auto moved = ops.perform(FileOperations::Action::Trash, legacy.fileName()); CTEST_CHECK_MESSAGE(moved, qPrintable(moved.error));
        // A reconciled legacy record carries the current native identity stamp.
        CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::Deleted, "legacy.txt", &error), qPrintable(error));
        // Simulate a file deleted by an older release, before the recovery journal existed.
        QDir journal(temp.filePath(".society-deleted"));
        for (const auto &name : journal.entryList({"*.json"}, QDir::Files)) CTEST_CHECK(journal.remove(name));
        const auto recovered = ops.perform(FileOperations::Action::Restore, moved.path);
        CTEST_CHECK_MESSAGE(recovered, qPrintable(recovered.error)); CTEST_COMPARE(recovered.path, legacy.fileName());
        const auto again = ops.perform(FileOperations::Action::Trash, recovered.path); CTEST_CHECK(again);
        const auto renamed = ops.perform(FileOperations::Action::Rename, again.path, "renamed.txt"); CTEST_CHECK_MESSAGE(renamed, qPrintable(renamed.error));
        const auto restored = ops.perform(FileOperations::Action::Restore, renamed.path);
        CTEST_CHECK_MESSAGE(restored, qPrintable(restored.error)); CTEST_COMPARE(restored.path, temp.filePath("Files/renamed.txt"));

        const auto protectedItem = ops.perform(FileOperations::Action::Trash, restored.path); CTEST_CHECK(protectedItem);
        CTEST_CHECK(QFile::rename(protectedItem.path, protectedItem.path + ".kept"));
        QFile replacement(protectedItem.path); CTEST_CHECK(replacement.open(QIODevice::WriteOnly)); replacement.write("different"); replacement.close();
        const auto mismatch = ops.perform(FileOperations::Action::Restore, protectedItem.path);
        CTEST_CHECK(!mismatch); CTEST_CHECK(mismatch.error.contains("no longer matches"));
        CTEST_CHECK(QFileInfo::exists(protectedItem.path)); CTEST_CHECK(QFileInfo::exists(protectedItem.path + ".kept"));
        for (const auto &name : journal.entryList({"*.json"}, QDir::Files)) CTEST_CHECK(journal.remove(name));
        const auto stale = ops.perform(FileOperations::Action::Restore, protectedItem.path);
        CTEST_CHECK(!stale); CTEST_CHECK(stale.error.contains("cannot be verified"));
        CTEST_CHECK(QFileInfo::exists(protectedItem.path)); CTEST_CHECK(QFileInfo::exists(protectedItem.path + ".kept"));
    }

    void rejectsRedirectedRecoveryJournalWithoutMovingSource() {
        QTemporaryDir temp(QDir::currentPath() + "/restore-journal-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        QTemporaryDir outside(QDir::currentPath() + "/restore-outside-XXXXXX.societycontainer"); CTEST_CHECK(outside.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive); FileOperations ops(*drive);
        QFile source(temp.filePath("Files/keep.txt")); CTEST_CHECK(source.open(QIODevice::WriteOnly)); source.write("keep"); source.close();
        CTEST_CHECK(createTestSymbolicLink(outside.path(), temp.filePath(".society-deleted")));
        const auto result = ops.perform(FileOperations::Action::Trash, source.fileName());
        CTEST_CHECK(!result); CTEST_CHECK(!result.error.isEmpty()); CTEST_CHECK(QFileInfo::exists(source.fileName()));
        CTEST_CHECK(!QFileInfo::exists(temp.filePath("Deleted/keep.txt")));
        CTEST_CHECK(QDir(outside.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }

    void refusesPartiallyAvailableReplicaWithoutDownloading() {
        QTemporaryDir temp(QDir::currentPath() + "/restore-replica-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto created = SocietyDrive::create(temp.path()); CTEST_CHECK(created);
        CTEST_CHECK(SocietyDrive::adoptReplicaIdentity(temp.path(), created->identifier(), created->identifier()));
        CTEST_CHECK(SocietyDrive::completeReplica(temp.path(), created->identifier()));
        const auto drive = SocietyDrive::open(temp.path()); CTEST_CHECK(drive);
        StorageMap map(*drive); CTEST_CHECK(!map.isLocalAuthority());
        CTEST_CHECK(QDir().mkpath(temp.filePath("Deleted/package")));
        QFile present(temp.filePath("Deleted/package/present.bin")); CTEST_CHECK(present.open(QIODevice::WriteOnly)); present.write("local"); present.close();
        CTEST_CHECK(map.publish({QJsonObject{{"path", "deleted/package/present.bin"}, {"kind", "file"}, {"size", "5"}, {"resident", true}},
                             QJsonObject{{"path", "deleted/package/missing.bin"}, {"kind", "file"}, {"size", "1"}, {"resident", false}}}));
        FileOperations ops(*drive);
        const auto result = ops.perform(FileOperations::Action::RestoreTo, temp.filePath("Deleted/package"), temp.filePath("Files"));
        CTEST_CHECK(!result); CTEST_CHECK(result.error.contains("not fully available"));
        CTEST_CHECK(QFileInfo::exists(present.fileName())); CTEST_CHECK(!QFileInfo::exists(temp.filePath("Files/package")));
        CTEST_CHECK(map.pendingRequests().isEmpty());
    }

    void ordinaryDirectoryWithoutDriveTerminates() {
        QTemporaryDir directory(QDir::currentPath() + "/ordinary-root-XXXXXX.societycontainer");
        CTEST_CHECK(directory.isValid());
        CTEST_CHECK(!FileOperations::containingDrive(directory.path()));
        QDir root(directory.path());
        while (!root.isRoot()) CTEST_CHECK(root.cdUp());
        CTEST_CHECK(!FileOperations::containingDrive(root.absolutePath()));
    }
    void temporaryLeaseMetadataIsProtectedAndNotCopied() {
        QTemporaryDir temp(QDir::currentPath() + "/metadata-filter-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive); FileOperations operations(*drive);
        CTEST_CHECK(QDir().mkdir(temp.filePath("Files/folder")));
        QFile sentinel(temp.filePath("Files/folder/.iisacc-temporary.lock")); CTEST_CHECK(sentinel.open(QIODevice::WriteOnly)); sentinel.close();
        QFile userLog(temp.filePath("Files/folder/user.log")); CTEST_CHECK(userLog.open(QIODevice::WriteOnly)); userLog.write("user data"); userLog.close();
        CTEST_CHECK(!operations.editable(sentinel.fileName()));
        CTEST_CHECK(!operations.perform(FileOperations::Action::Trash, sentinel.fileName()));
        CTEST_CHECK(!operations.perform(FileOperations::Action::Rename, sentinel.fileName(), "renamed"));
        const auto copied = operations.perform(FileOperations::Action::Duplicate, temp.filePath("Files/folder"));
        CTEST_CHECK_MESSAGE(copied, qPrintable(copied.error));
        // The source lease is excluded by copyTree. Publishing the new file's
        // classification can independently create a fresh zero-byte lease.
        const QFileInfo copiedLease(copied.path + "/.iisacc-temporary.lock");
        if (copiedLease.exists()) { CTEST_CHECK(copiedLease.isFile()); CTEST_CHECK(!copiedLease.isSymLink()); CTEST_COMPARE(copiedLease.size(), qint64(0)); }
        CTEST_CHECK(QFileInfo::exists(copied.path + "/user.log"));
        CTEST_COMPARE(QFileInfo(sentinel.fileName()).size(), qint64(0));
    }
    void createsDirectoriesAndMovesCompleteTrees() {
        QTemporaryDir temp(QDir::currentPath() + "/tree-manage-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        QTemporaryDir outside(QDir::currentPath() + "/tree-outside-XXXXXX.societycontainer"); CTEST_CHECK(outside.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive); FileOperations ops(*drive);
        const auto project = ops.createDirectory(temp.filePath("Files"), QString::fromUtf8("자료 Project"));
        CTEST_CHECK_MESSAGE(project, qPrintable(project.error));
        const auto sub = ops.createDirectory(project.path, "nested"); CTEST_CHECK(sub);
        QFile file(sub.path + "/note.txt"); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("preserved"); file.close();
        CTEST_CHECK(!ops.createDirectory(temp.filePath("Files"), QString::fromUtf8("자료 Project")));
        for (const auto& name : {"", ".", "..", "../escape", "a/b", "a\\b", "scheme:name", ".society-sync"})
            CTEST_CHECK(!ops.createDirectory(project.path, name));
        CTEST_CHECK(!ops.createDirectory(temp.path(), "protected"));
        CTEST_CHECK(!ops.createDirectory(outside.path(), "escape"));
        CTEST_CHECK(!ops.createDirectory(file.fileName(), "child"));
        CTEST_CHECK(!ops.perform(FileOperations::Action::Move, project.path, sub.path));
        CTEST_CHECK(!ops.perform(FileOperations::Action::Move, project.path, outside.path()));
        const auto destination = ops.createDirectory(temp.filePath("Files"), "archive"); CTEST_CHECK(destination);
        const auto moved = ops.perform(FileOperations::Action::Move, project.path, destination.path);
        CTEST_CHECK_MESSAGE(moved, qPrintable(moved.error)); CTEST_CHECK(!QFileInfo::exists(project.path));
        QFile preserved(moved.path + "/nested/note.txt"); CTEST_CHECK(preserved.open(QIODevice::ReadOnly));
        CTEST_COMPARE(preserved.readAll(), QByteArray("preserved")); preserved.close();
        const auto other = ops.createDirectory(temp.filePath("Files"), QString::fromUtf8("자료 Project")); CTEST_CHECK(other);
        CTEST_CHECK(!ops.perform(FileOperations::Action::Move, other.path, destination.path));
        CTEST_CHECK(QFileInfo::exists(other.path)); CTEST_CHECK(QFileInfo::exists(moved.path + "/nested/note.txt"));
        CTEST_CHECK(!ops.perform(FileOperations::Action::Move, temp.filePath("Models"), destination.path));
        const auto crossSection = ops.perform(FileOperations::Action::Move, moved.path, temp.filePath("Forked"));
        CTEST_CHECK_MESSAGE(crossSection, qPrintable(crossSection.error));
        const auto updated = drive->tree("Forked"); CTEST_CHECK_MESSAGE(updated, updated.error.message().c_str());
        CTEST_COMPARE(updated.value->children.size(), std::size_t(1));
        const auto nativeRelative = updated.value->children.front().relativePath / "nested/note.txt";
        const auto* movedNode = updated.value->find(nativeRelative); CTEST_CHECK(movedNode);
        CTEST_COMPARE(movedNode->size, std::uintmax_t(9));
        const auto typed = drive->entry(QString::fromUtf8("Forked/자료 Project/nested/note.txt"));
        CTEST_CHECK_MESSAGE(typed, typed.error.message().c_str());
        CTEST_CHECK(updated.value->find(typed.value->relativePath));
        CTEST_CHECK(QFile::link(outside.path(), temp.filePath("Files/link")));
        CTEST_CHECK(!ops.createDirectory(temp.filePath("Files/link"), "escape"));
        CTEST_CHECK(!ops.perform(FileOperations::Action::Move, crossSection.path, temp.filePath("Files/link")));
    }

    void deletionBypassesContentAndReplicationWork() {
        QTemporaryDir temp(QDir::currentPath() + "/direct-delete-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        StorageMap map(*drive); FileOperations ops(*drive);
        const auto source = temp.filePath("Models/Checkpoint/large.safetensors");
        constexpr qint64 bytes = 8LL * 1024 * 1024 * 1024;
        CTEST_CHECK(QDir().mkpath(temp.filePath("Models/Checkpoint")));
        QFile file(source); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_CHECK(file.resize(bytes)); file.close();
        // A stale map must neither download nor prevent moving the actual file.
        const QJsonObject entry{{"path", "models/Checkpoint/large.safetensors"}, {"kind", "file"},
            {"size", "1"}, {"resident", false}, {"version", QString(64, 'a')}};
        CTEST_CHECK(map.publish({entry}));
        QLockFile sync(temp.filePath(".society-sync/operation.lock")); CTEST_CHECK(sync.tryLock());
        QElapsedTimer elapsed; elapsed.start();
        const auto moved = ops.perform(FileOperations::Action::Trash, source);
        CTEST_CHECK_MESSAGE(moved, qPrintable(moved.error));
        qInfo() << "8 GiB trash rename (ms):" << elapsed.elapsed();
        CTEST_CHECK(!QFileInfo::exists(source)); CTEST_COMPARE(QFileInfo(moved.path).size(), bytes);
        auto deleted = entry; deleted["path"] = "deleted/large.safetensors";
        CTEST_CHECK(map.publish({entry, deleted})); elapsed.restart();
        const auto removed = ops.perform(FileOperations::Action::Remove, moved.path);
        CTEST_CHECK_MESSAGE(removed, qPrintable(removed.error));
        qInfo() << "8 GiB permanent removal (ms):" << elapsed.elapsed();
        CTEST_CHECK(!QFileInfo::exists(moved.path)); CTEST_CHECK(map.pendingRequests().isEmpty());
        CTEST_COMPARE(map.objects(), QJsonArray({entry, deleted})); // No graph/catalog mutation in the deletion path.
    }
    void removesPartialPackagesWithoutResolvingMissingChildren() {
        QTemporaryDir temp(QDir::currentPath() + "/partial-delete-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        QTemporaryDir outside(QDir::currentPath() + "/delete-keep-XXXXXX.societycontainer"); CTEST_CHECK(outside.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive); StorageMap map(*drive);
        CTEST_CHECK(QDir().mkpath(temp.filePath("Models/partial/sub")));
        QFile keep(outside.filePath("keep.txt")); CTEST_CHECK(keep.open(QIODevice::WriteOnly)); keep.write("keep"); keep.close();
        CTEST_CHECK(QFile::copy(keep.fileName(), temp.filePath("Models/partial/sub/present.bin")));
        CTEST_CHECK(createTestSymbolicLink(outside.path(), temp.filePath("Models/partial/sub/link")));
        CTEST_CHECK(map.publish({QJsonObject{{"path", "models/partial/missing.bin"}, {"kind", "file"}, {"resident", false}}}));
        FileOperations ops(*drive);
        const auto moved = ops.perform(FileOperations::Action::Trash, temp.filePath("Models/partial"));
        CTEST_CHECK_MESSAGE(moved, qPrintable(moved.error));
        CTEST_CHECK(QFileInfo::exists(moved.path + "/sub/present.bin"));
        CTEST_CHECK(ops.perform(FileOperations::Action::Remove, moved.path));
        CTEST_CHECK(!QFileInfo::exists(moved.path)); CTEST_CHECK(QFileInfo::exists(keep.fileName()));
        CTEST_CHECK(map.pendingRequests().isEmpty());
    }
    void handlesFilesPackagesAndCollisions() {
        QTemporaryDir temp(QDir::currentPath() + "/file-actions-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        FileOperations ops(*drive);
        const auto path = temp.filePath("Models/model.safetensors");
        QFile f(path); CTEST_CHECK(f.open(QIODevice::WriteOnly)); f.write("model bytes"); f.close();
        auto first = ops.perform(FileOperations::Action::Duplicate, path); CTEST_CHECK_MESSAGE(first, qPrintable(first.error));
        auto second = ops.perform(FileOperations::Action::Duplicate, path); CTEST_CHECK(second); CTEST_CHECK(first.path != second.path);
        QFile copy(first.path); CTEST_CHECK(copy.open(QIODevice::ReadOnly)); CTEST_COMPARE(copy.readAll(), QByteArray("model bytes")); copy.close();
        auto renamed = ops.perform(FileOperations::Action::Rename, first.path, "renamed.safetensors"); CTEST_CHECK(renamed); CTEST_CHECK(!QFileInfo::exists(first.path));
        CTEST_CHECK(!ops.perform(FileOperations::Action::Rename, renamed.path, "model.safetensors"));
        CTEST_CHECK(QDir().mkpath(temp.filePath("Models/package/sub")));
        CTEST_CHECK(QFile::copy(path, temp.filePath("Models/package/sub/weight.bin")));
        auto package = ops.perform(FileOperations::Action::Duplicate, temp.filePath("Models/package")); CTEST_CHECK(package);
        CTEST_CHECK(QFileInfo::exists(package.path + "/sub/weight.bin"));
        auto trashed = ops.perform(FileOperations::Action::Trash, temp.filePath("Models/package")); CTEST_CHECK(trashed);
        CTEST_CHECK(trashed.path.startsWith(temp.filePath("Deleted/"))); CTEST_CHECK(QFileInfo::exists(trashed.path + "/sub/weight.bin"));
        auto restored = ops.perform(FileOperations::Action::Restore, trashed.path); CTEST_CHECK_MESSAGE(restored, qPrintable(restored.error));
        CTEST_CHECK(!QFileInfo::exists(trashed.path));
        CTEST_CHECK(QFileInfo::exists(restored.path + "/sub/weight.bin"));
    }
    void protectsLayoutAndRejectsEscapes() {
        QTemporaryDir temp(QDir::currentPath() + "/file-action-boundary-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        QTemporaryDir outside(QDir::currentPath() + "/file-action-outside-XXXXXX.societycontainer"); CTEST_CHECK(outside.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive); FileOperations ops(*drive);
        for (const auto &path : {temp.path(), temp.filePath("Models"), temp.filePath("Files")}) {
            CTEST_CHECK(!ops.editable(path)); CTEST_CHECK(!ops.perform(FileOperations::Action::Trash, path));
        }
        for (const auto *name : {"Documents", "Audios", "3D objects"}) {
            const auto path = temp.filePath("Files/" + QString(name));
            CTEST_CHECK(QDir().mkdir(path)); CTEST_CHECK(ops.editable(path));
            const auto renamed = ops.perform(FileOperations::Action::Rename, path, QString(name) + " renamed");
            CTEST_CHECK_MESSAGE(renamed, qPrintable(renamed.error));
            CTEST_CHECK(ops.perform(FileOperations::Action::Trash, renamed.path));
        }
        CTEST_CHECK(QDir().mkdir(temp.filePath("Files/Documents")));
        QFile f(temp.filePath("Files/example.txt")); CTEST_CHECK(f.open(QIODevice::WriteOnly)); f.write("keep"); f.close();
        for (const auto &name : {"../escape", ".society-hidden", "a/b", "", "."})
            CTEST_CHECK(!ops.perform(FileOperations::Action::Rename, f.fileName(), name));
        CTEST_CHECK(!ops.perform(FileOperations::Action::Copy, f.fileName(), outside.path()));
        QFile external(outside.filePath("from-finder.txt")); CTEST_CHECK(external.open(QIODevice::WriteOnly)); external.write("external"); external.close();
        const auto pasted = ops.perform(FileOperations::Action::Copy, external.fileName(), temp.filePath("Files/Documents"));
        CTEST_CHECK_MESSAGE(pasted, qPrintable(pasted.error)); CTEST_CHECK(QFileInfo::exists(external.fileName()));
        CTEST_CHECK(!ops.perform(FileOperations::Action::Remove, f.fileName())); // Only Deleted allows permanent removal.
        CTEST_CHECK(createTestSymbolicLink(outside.path(), temp.filePath("Models/link")));
        CTEST_CHECK(!ops.perform(FileOperations::Action::Trash, temp.filePath("Models/link")));
        CTEST_CHECK(createTestSymbolicLink(outside.path(), temp.filePath("Deleted/link")));
        CTEST_CHECK(!ops.perform(FileOperations::Action::Copy, f.fileName(), temp.filePath("Deleted/link")));
        CTEST_CHECK(QFileInfo::exists(f.fileName())); CTEST_CHECK(QFileInfo::exists(temp.filePath("Files/Documents")));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name" << std::endl; return 1; }
    society_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    FileOperationsTests tests;
    runner.run("classificationIsPrivateAcrossFileOperations", [&] { tests.classificationIsPrivateAcrossFileOperations(); });
    runner.run("modelCategoryKeepsPrivateClassification", [&] { tests.modelCategoryKeepsPrivateClassification(); });
    runner.run("movesProjectPackagesAsSingleItems", [&] { tests.movesProjectPackagesAsSingleItems(); });
    runner.run("relocationParticipantRunsWithinTheMove", [&] { tests.relocationParticipantRunsWithinTheMove(); });
    runner.run("caseOnlyRenameKeepsTheItem", [&] { tests.caseOnlyRenameKeepsTheItem(); });
    runner.run("relocationFailureReportsWhichSideMoved", [&] { tests.relocationFailureReportsWhichSideMoved(); });
    runner.run("deletedLocksSeparateIndependentAssetsAndProtectRelatedPaths", [&] { tests.deletedLocksSeparateIndependentAssetsAndProtectRelatedPaths(); });
    runner.run("providerControlsCannotBeManipulatedAsFiles", [&] { tests.providerControlsCannotBeManipulatedAsFiles(); });
    runner.run("orphanedDestinationMetadataIsNeverAdopted", [&] { tests.orphanedDestinationMetadataIsNeverAdopted(); });
    runner.run("stagesAndRecoversCompleteAssetGroupsAcrossVolumes", [&] { tests.stagesAndRecoversCompleteAssetGroupsAcrossVolumes(); });
    runner.run("movesAndRestoresFileClassificationWithTheAsset", [&] { tests.movesAndRestoresFileClassificationWithTheAsset(); });
    runner.run("duplicatesModelAndItsAssociatedAssetWithDistinctIdentity", [&] { tests.duplicatesModelAndItsAssociatedAssetWithDistinctIdentity(); });
    runner.run("importingModelCopiesRecognizedCompanions", [&] { tests.importingModelCopiesRecognizedCompanions(); });
    runner.run("nativeRenameNeverReplacesAnExistingItem", [&] { tests.nativeRenameNeverReplacesAnExistingItem(); });
    runner.run("restoresDeletedItemsWithoutOverwritingAndKeepsOrigins", [&] { tests.restoresDeletedItemsWithoutOverwritingAndKeepsOrigins(); });
    runner.run("renamedAndLegacyDeletedItemsKeepTrustworthyOrigins", [&] { tests.renamedAndLegacyDeletedItemsKeepTrustworthyOrigins(); });
    runner.run("rejectsRedirectedRecoveryJournalWithoutMovingSource", [&] { tests.rejectsRedirectedRecoveryJournalWithoutMovingSource(); });
    runner.run("refusesPartiallyAvailableReplicaWithoutDownloading", [&] { tests.refusesPartiallyAvailableReplicaWithoutDownloading(); });
    runner.run("ordinaryDirectoryWithoutDriveTerminates", [&] { tests.ordinaryDirectoryWithoutDriveTerminates(); });
    runner.run("temporaryLeaseMetadataIsProtectedAndNotCopied", [&] { tests.temporaryLeaseMetadataIsProtectedAndNotCopied(); });
    runner.run("createsDirectoriesAndMovesCompleteTrees", [&] { tests.createsDirectoriesAndMovesCompleteTrees(); });
    runner.run("deletionBypassesContentAndReplicationWork", [&] { tests.deletionBypassesContentAndReplicationWork(); });
    runner.run("removesPartialPackagesWithoutResolvingMissingChildren", [&] { tests.removesPartialPackagesWithoutResolvingMissingChildren(); });
    runner.run("handlesFilesPackagesAndCollisions", [&] { tests.handlesFilesPackagesAndCollisions(); });
    runner.run("protectsLayoutAndRejectsEscapes", [&] { tests.protectsLayoutAndRejectsEscapes(); });
    return runner.result();
}
