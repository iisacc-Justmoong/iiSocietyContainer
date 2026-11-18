#include "ModelImportService.h"
#include "SocietyDrive.h"
#include "SharedStorage.h"
#include "ModelStore.h"
#include "FileOperations.h"
#include <FileCatalog.h>
#include <TemporaryLease.h>
#include <iiFileProviderPresentation.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include "SignalRecorder.h"
#include "QtEventWait.h"
#include <QEvent>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtEndian>

using namespace iiSocietyContainer;
namespace {
QString classificationPath(const QString &container, const QString &asset) {
    for (const auto section : allStoreSections()) {
        const auto root = container + '/' + storeSectionName(section) + '/';
        if (asset.startsWith(root))
            return container + "/.society-runtime/classifications/" + storeSectionKey(section)
                + '/' + asset.mid(root.size()) + ".iisacc.json";
    }
    return {};
}
}

class ModelImportServiceTests final : public QObject {
public:
    void repairsMissingCategoriesWhenAContainerIsSelected() {
        QTemporaryDir fixture(QDir::tempPath() + "/model-layout-XXXXXX.societycontainer");
        CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        const auto manifest = fixture.filePath(".society-drive.json");
        QFile before(manifest); CTEST_CHECK(before.open(QIODevice::ReadOnly)); const auto manifestBytes=before.readAll(); before.close();
        for (const auto &name : {QStringLiteral("VLM"), QStringLiteral("Workflows"), QStringLiteral("Aesthetic Gradient")})
            if (QFileInfo::exists(fixture.filePath("Models/" + name))) CTEST_CHECK(QDir().rmdir(fixture.filePath("Models/" + name)));
        const auto asset = fixture.filePath("Models/Checkpoint/preserved.safetensors");
        CTEST_CHECK(QDir().mkpath(QFileInfo(asset).absolutePath()));
        QFile model(asset); CTEST_CHECK(model.open(QIODevice::WriteOnly)); CTEST_COMPARE(model.write("preserve"), qint64(8)); model.close();
        ModelImportService service;
        society_ctest::SignalRecorder organized(&service, &ModelImportService::organized);
        service.setContainerPath(fixture.path());
        CTEST_WAIT_VERIFY(!organized.isEmpty(), 30000);
        for (const auto type : allModelTypes()) CTEST_CHECK_MESSAGE(QFileInfo(fixture.filePath("Models/" + modelTypeName(type))).isDir(), qPrintable(modelTypeName(type)));
        CTEST_CHECK(model.open(QIODevice::ReadOnly)); CTEST_COMPARE(model.readAll(), QByteArray("preserve"));
        CTEST_CHECK(before.open(QIODevice::ReadOnly)); CTEST_COMPARE(before.readAll(), manifestBytes);
        CTEST_COMPARE(SocietyDrive::open(fixture.path())->identifier(), drive->identifier());
    }
    void catalogOnlyModelSearchAndIdentityConflictsNeedNoObjectStore() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-model-catalog-XXXXXX");
        CTEST_CHECK(workspace.isValid());
        const auto root = workspace.filePath("drive.societycontainer");
        CTEST_CHECK(QDir().mkdir(root)); const auto drive = SocietyDrive::create(root); CTEST_CHECK(drive);
        CTEST_CHECK(QDir().mkpath(root + "/Models/Other"));
        const auto path = root + "/Models/Other/creative.safetensors";
        const auto replacement = workspace.filePath("replacement");
        QFile original(path); CTEST_CHECK(original.open(QIODevice::WriteOnly)); CTEST_COMPARE(original.write("original"), qint64(8)); original.close();
        QFile other(replacement); CTEST_CHECK(other.open(QIODevice::WriteOnly)); CTEST_COMPARE(other.write("different"), qint64(9)); other.close();
        const auto store = ModelStore::open(root); CTEST_CHECK(store);
        const auto storage = SharedStorage::open(root); CTEST_CHECK(storage);
        QString error;
        const auto id = store->indexAsset(path, &error); CTEST_CHECK_MESSAGE(!id.isEmpty(), qPrintable(error));
        const auto identifier = drive->storageKind() == SocietyDrive::StorageKind::LogicalDisk
            ? drive->storageIdentifier() : drive->identifier();
        const auto package = FileRepository::packagePath(std::filesystem::path(root.toStdU16String()), identifier.toStdString());
        CTEST_CHECK(!std::filesystem::exists(package / "objects.sqlite3"));
        const auto tracking = storage->fileTracking(StoreSection::Models, "Other/creative.safetensors", &error);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        CTEST_COMPARE(tracking.value("resource").toObject().value("id").toString(), id);
        const auto results = storage->searchFiles({{"section", "models"}, {"text", "creative"}}, &error);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        CTEST_COMPARE(results.value("files").toArray().size(), 1);
        CTEST_COMPARE(results.value("files").toArray().first().toObject().value("resourceId").toString(), id);
        const auto unicodeRelative = QString::fromUtf8("Other/모델.safetensors");
        const auto unicodePath = root + "/Models/" + unicodeRelative;
        QFile unicode(unicodePath); CTEST_CHECK(unicode.open(QIODevice::WriteOnly)); CTEST_COMPARE(unicode.write("weights"), qint64(7)); unicode.close();
        const auto unicodeId = store->indexAsset(unicodePath, &error); CTEST_CHECK_MESSAGE(!unicodeId.isEmpty(), qPrintable(error));
        const auto unicodeTracking = storage->fileTracking(StoreSection::Models, unicodeRelative, &error);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        CTEST_COMPARE(unicodeTracking.value("resource").toObject().value("id").toString(), unicodeId);
#ifdef Q_OS_MACOS
        const auto decomposed = storage->fileTracking(StoreSection::Models, unicodeRelative.normalized(QString::NormalizationForm_D), &error);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        CTEST_COMPARE(decomposed.value("resource").toObject().value("id").toString(), unicodeId);
#endif
        CTEST_CHECK(QFile::remove(path)); CTEST_CHECK(QFile::rename(replacement, path));
        CTEST_CHECK(store->indexAsset(path, &error).isEmpty());
        CTEST_CHECK(error.contains("different file", Qt::CaseInsensitive));
        CTEST_COMPARE(storage->fileTracking(StoreSection::Models, "Other/creative.safetensors").value("resourceStatus").toString(), QStringLiteral("stale"));
        QFile untouched(path); CTEST_CHECK(untouched.open(QIODevice::ReadOnly)); CTEST_COMPARE(untouched.readAll(), QByteArray("different"));
        CTEST_CHECK(!std::filesystem::exists(package / "objects.sqlite3"));
    }

    void importsDirectlyWithoutWaitingForOrDuplicatingTheObjectStore() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-direct-models-XXXXXX");
        CTEST_CHECK(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        CTEST_CHECK(QDir().mkdir(drivePath));
        const auto drive = SocietyDrive::create(drivePath); CTEST_CHECK(drive);
        const auto storage = SharedStorage::open(drivePath); CTEST_CHECK(storage);
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{4096}}, {"data_offsets", QJsonArray{0, 4096}}}}}).toJson(QJsonDocument::Compact);
        QByteArray data(8, '\0'); qToLittleEndian(quint64(header.size()), data.data()); data += header + QByteArray(4096, 'm');
        QList<ModelImportSource> inputs;
        for (const auto &name : {QStringLiteral("first.safetensors"), QStringLiteral("second.safetensors")}) {
            const auto path = workspace.filePath(name);
            QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write(data), data.size()); file.close();
            inputs.append({name, [path](const auto &consume, const auto &) { consume(path); return QString(); }, path});
        }
        ModelImportService service;
        society_ctest::SignalRecorder organized(&service, &ModelImportService::organized); society_ctest::SignalRecorder finished(&service, &ModelImportService::finished);
        service.setContainerPath(drivePath);
        CTEST_WAIT_VERIFY(!organized.isEmpty(), 30000);
        auto repository = storage->fileRepository();
        const auto identifier = drive->storageKind() == SocietyDrive::StorageKind::LogicalDisk
            ? drive->storageIdentifier() : drive->identifier();
        const auto package = FileRepository::packagePath(std::filesystem::path(drivePath.toStdU16String()), identifier.toStdString());
        std::optional<iiFileProvider::TemporaryLease> occupied;
        occupied.emplace(package / "operations", iiFileProvider::TemporaryLease::Mode::TryExclusive);
        CTEST_CHECK(occupied->locked());
        CTEST_CHECK(service.importSources(inputs));
        CTEST_WAIT_COMPARE_TIMEOUT(finished.size(), 1, 20000);
        CTEST_CHECK_MESSAGE(service.errorString().isEmpty(), qPrintable(service.errorString()));
        CTEST_CHECK(occupied->locked());
        const auto paths = finished.first().at(1).toStringList();
        CTEST_COMPARE(paths.size(), 2);
        QStringList ids;
        for (const auto &path : paths) {
            QFile file(path); CTEST_CHECK(file.open(QIODevice::ReadOnly)); CTEST_COMPARE(file.readAll(), data);
            const auto relative = QDir(drivePath + "/Models").relativeFilePath(path);
            const auto tracking = storage->fileTracking(StoreSection::Models, relative);
            CTEST_COMPARE(tracking.value("resourceStatus").toString(), QStringLiteral("working-copy-only"));
            CTEST_CHECK(tracking.value("objectKey").toString().isEmpty());
            CTEST_CHECK(tracking.value("rawKey").toString().isEmpty());
            const auto id = tracking.value("resource").toObject().value("id").toString();
            CTEST_CHECK(!id.isEmpty()); ids.append(id);
            CTEST_CHECK(!QFileInfo::exists(path + ".iisacc.json"));
            CTEST_CHECK(QFileInfo::exists(classificationPath(drivePath, path)));
        }
        CTEST_CHECK(ids[0] != ids[1]);
        occupied.reset(); repository.reset();
        QString error;
        CTEST_CHECK_MESSAGE(storage->reconcileFiles(&error), qPrintable(error));
        auto readOnly = storage->fileRepository({}, FileRepository::Access::ReadOnly);
        for (const auto &path : paths) {
            const auto relative = QDir(drivePath + "/Models").relativeFilePath(path);
            CTEST_CHECK(!readOnly->lookup(storage->managedFilePath(StoreSection::Models, relative)));
        }
        const auto result = storage->searchFiles({{"section", "models"}, {"text", "first"}}, &error);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        CTEST_COMPARE(result.value("files").toArray().size(), 1);
        readOnly.reset();
        const auto renamed = FileOperations(*drive).perform(FileOperations::Action::Rename, paths[0], "renamed.safetensors");
        CTEST_CHECK_MESSAGE(renamed, qPrintable(renamed.error));
        const auto movedTracking = storage->fileTracking(StoreSection::Models,
            QDir(drivePath + "/Models").relativeFilePath(renamed.path));
        CTEST_COMPARE(movedTracking.value("resource").toObject().value("id").toString(), ids[0]);
        const auto trashed = FileOperations(*drive).perform(FileOperations::Action::Trash, renamed.path);
        CTEST_CHECK_MESSAGE(trashed, qPrintable(trashed.error));
        CTEST_CHECK_MESSAGE(storage->reconcileFiles(&error), qPrintable(error));
        auto afterTrash = storage->fileRepository({}, FileRepository::Access::ReadOnly);
        CTEST_CHECK(!afterTrash->lookup(storage->managedFilePath(StoreSection::Deleted,
            QDir(drivePath + "/Deleted").relativeFilePath(trashed.path))));
        afterTrash.reset();
        const auto replacementSource = workspace.filePath("renamed.safetensors");
        QFile replacement(replacementSource); CTEST_CHECK(replacement.open(QIODevice::WriteOnly));
        CTEST_COMPARE(replacement.write(data), data.size()); replacement.close();
        ModelImportSource replacementInput{QStringLiteral("renamed.safetensors"),
            [replacementSource](const auto &consume, const auto &) { consume(replacementSource); return QString(); }, replacementSource};
        CTEST_CHECK(service.importSources({replacementInput}));
        CTEST_WAIT_COMPARE_TIMEOUT(finished.size(), 2, 30000);
        CTEST_CHECK_MESSAGE(service.errorString().isEmpty(), qPrintable(service.errorString()));
        const auto newPath = finished.last().at(1).toStringList().value(0);
        CTEST_CHECK(!newPath.isEmpty());
        const auto newTracking = storage->fileTracking(StoreSection::Models,
            QDir(drivePath + "/Models").relativeFilePath(newPath));
        CTEST_CHECK(newTracking.value("resource").toObject().value("id").toString() != ids[0]);
        const auto restored = FileOperations(*drive).perform(FileOperations::Action::Restore, trashed.path);
        CTEST_CHECK_MESSAGE(restored, qPrintable(restored.error));
        const auto restoredTracking = storage->fileTracking(StoreSection::Models,
            QDir(drivePath + "/Models").relativeFilePath(restored.path));
        CTEST_COMPARE(restoredTracking.value("resource").toObject().value("id").toString(), ids[0]);
        QFile restoredFile(restored.path); CTEST_CHECK(restoredFile.open(QIODevice::ReadOnly));
        CTEST_COMPARE(restoredFile.readAll(), data);
        restoredFile.close();
        society_ctest::SignalRecorder recategorized(&service, &ModelImportService::recategorized);
        CTEST_CHECK(service.recategorize(restored.path, QStringLiteral("LLM")));
        CTEST_WAIT_COMPARE_TIMEOUT(recategorized.size(), 1, 30000);
        CTEST_CHECK_MESSAGE(service.errorString().isEmpty(), qPrintable(service.errorString()));
        const auto recategorizedPath = recategorized.first().at(2).toString();
        const auto recategorizedTracking = storage->fileTracking(StoreSection::Models,
            QDir(drivePath + "/Models").relativeFilePath(recategorizedPath));
        CTEST_COMPARE(recategorizedTracking.value("resource").toObject().value("id").toString(), ids[0]);
        CTEST_CHECK(!QFileInfo::exists(recategorizedPath + ".iisacc.json"));
        CTEST_CHECK(QFileInfo::exists(classificationPath(drivePath, recategorizedPath)));
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }

    void staleEmptyOperationLockDoesNotBlockMultipleImports() {
#ifdef Q_OS_ANDROID
        society_ctest::skip("Android does not recover an ownerless lock without a cross-thread native lock guarantee.");
#endif
        QTemporaryDir workspace(QDir::tempPath() + "/container-stale-import-XXXXXX");
        CTEST_CHECK(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        CTEST_CHECK(QDir().mkdir(drivePath));
        CTEST_CHECK(SocietyDrive::create(drivePath));
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{4}}, {"data_offsets", QJsonArray{0, 4}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data());
        bytes += header + QByteArray("test");
        QList<ModelImportSource> sources;
        for (int index = 0; index < 3; ++index) {
            const auto name = QStringLiteral("model%1.safetensors").arg(index);
            const auto path = workspace.filePath(name);
            QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write(bytes), bytes.size()); file.close();
            sources.append({name, [path](const auto &consume, const auto &) { consume(path); return QString(); }, path});
        }
        ModelImportService service;
        society_ctest::SignalRecorder finished(&service, &ModelImportService::finished);
        society_ctest::SignalRecorder organized(&service, &ModelImportService::organized);
        bool sawBatchProgress = false;
        connect(&service, &ModelImportService::stateChanged, &service, [&] {
            sawBatchProgress |= service.status().contains(QStringLiteral("1 of 3"));
        });
        service.setContainerPath(drivePath);
        CTEST_WAIT_VERIFY(!organized.isEmpty(), 30000);
        CTEST_CHECK(QDir().mkpath(drivePath + "/.society-sync"));
        QFile abandoned(drivePath + "/.society-sync/operation.lock");
        CTEST_CHECK(abandoned.open(QIODevice::WriteOnly));
        CTEST_CHECK(abandoned.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-600), QFileDevice::FileModificationTime));
        abandoned.close();
        CTEST_CHECK(service.importSources(sources));
        CTEST_WAIT_COMPARE_TIMEOUT(finished.size(), 1, 20000);
        CTEST_CHECK_MESSAGE(service.errorString().isEmpty(), qPrintable(service.errorString()));
        CTEST_CHECK(sawBatchProgress);
        const auto paths = finished.first().at(1).toStringList();
        CTEST_COMPARE(paths.size(), sources.size());
        for (const auto &path : paths) {
            QFile imported(path); CTEST_CHECK(imported.open(QIODevice::ReadOnly)); CTEST_COMPARE(imported.readAll(), bytes);
        }
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }

    void liveOperationLockIsNotRemovedAndWaitingIsVisible(bool malformed) {
        QTemporaryDir workspace(QDir::tempPath() + "/container-active-import-XXXXXX");
        CTEST_CHECK(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        CTEST_CHECK(QDir().mkdir(drivePath)); CTEST_CHECK(SocietyDrive::create(drivePath));
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{1}}, {"data_offsets", QJsonArray{0, 1}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data()); bytes += header + QByteArray("x");
        const auto path = workspace.filePath("model.safetensors");
        QFile source(path); CTEST_CHECK(source.open(QIODevice::WriteOnly)); CTEST_COMPARE(source.write(bytes), bytes.size()); source.close();
        ModelImportService service;
        society_ctest::SignalRecorder finished(&service, &ModelImportService::finished);
        society_ctest::SignalRecorder organized(&service, &ModelImportService::organized);
        bool waiting = false;
        connect(&service, &ModelImportService::stateChanged, &service, [&] {
            waiting |= service.status().contains(QStringLiteral("Waiting for another container operation"));
        });
        service.setContainerPath(drivePath);
        CTEST_WAIT_VERIFY(!organized.isEmpty(), 30000);
        CTEST_CHECK(QDir().mkpath(drivePath + "/.society-sync"));
        QLockFile owner(drivePath + "/.society-sync/operation.lock"); owner.setStaleLockTime(0); CTEST_CHECK(owner.tryLock());
        if (malformed) {
            QFile lockContents(owner.fileName()); CTEST_CHECK(lockContents.open(QIODevice::ReadWrite));
            CTEST_CHECK(lockContents.resize(0));
            CTEST_CHECK(lockContents.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-600), QFileDevice::FileModificationTime));
            lockContents.close();
        }
        ModelImportSource input{QStringLiteral("model.safetensors"),
            [path](const auto &consume, const auto &) { consume(path); return QString(); }, path};
        CTEST_CHECK(service.importSources({input}));
        CTEST_WAIT_VERIFY(waiting, 5000);
        society_ctest::processFor(1000);
        CTEST_CHECK(finished.isEmpty());
        CTEST_CHECK(owner.isLocked());
        CTEST_CHECK(QFileInfo::exists(drivePath + "/.society-sync/operation.lock"));
        if (malformed) CTEST_COMPARE(QFileInfo(owner.fileName()).size(), qint64(0));
        owner.unlock();
        CTEST_WAIT_COMPARE_TIMEOUT(finished.size(), 1, 30000);
        CTEST_CHECK_MESSAGE(service.errorString().isEmpty(), qPrintable(service.errorString()));
        CTEST_COMPARE(finished.first().at(1).toStringList().size(), 1);
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }

    void publicationTimeoutPreservesRetryableBatchWithoutCopyingTheRest() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-blocked-import-XXXXXX");
        CTEST_CHECK(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        CTEST_CHECK(QDir().mkdir(drivePath)); CTEST_CHECK(SocietyDrive::create(drivePath));
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{1}}, {"data_offsets", QJsonArray{0, 1}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data()); bytes += header + QByteArray("x");
        std::atomic_int reads{0};
        QList<ModelImportSource> sources;
        for (int index = 0; index < 3; ++index) {
            const auto name = QStringLiteral("model%1.safetensors").arg(index);
            const auto path = workspace.filePath(name);
            QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write(bytes), bytes.size()); file.close();
            sources.append({name, [path, &reads](const auto &consume, const auto &) {
                ++reads; consume(path); return QString();
            }, path});
        }
        ModelImportService service;
        society_ctest::SignalRecorder finished(&service, &ModelImportService::finished);
        society_ctest::SignalRecorder organized(&service, &ModelImportService::organized);
        service.setContainerPath(drivePath);
        CTEST_WAIT_VERIFY(!organized.isEmpty(), 30000);
        CTEST_CHECK(QDir().mkpath(drivePath + "/.society-sync"));
        QLockFile owner(drivePath + "/.society-sync/operation.lock"); owner.setStaleLockTime(0); CTEST_CHECK(owner.tryLock());
        CTEST_CHECK(service.importSources(sources));
        const auto jobId = service.currentJobId();
        CTEST_WAIT_COMPARE_TIMEOUT(finished.size(), 1, 45000);
        CTEST_COMPARE(reads.load(), 1);
        CTEST_CHECK(service.errorString().contains(QStringLiteral("publication lock")));
        CTEST_CHECK(finished.first().at(1).toStringList().isEmpty());
        CTEST_CHECK(owner.isLocked()); owner.unlock();
        service.selectJob(jobId);
        CTEST_WAIT_COMPARE_TIMEOUT(service.jobItems().size(), 3, 10000);
        for (const auto &value : service.jobItems()) {
            const auto item = value.toMap();
            CTEST_COMPARE(item.value("state").toString(), QStringLiteral("failed"));
            CTEST_CHECK(item.value("retryable").toBool());
        }
        CTEST_CHECK(service.retryFailed(jobId));
        CTEST_WAIT_COMPARE_TIMEOUT(finished.size(), 2, 30000);
        CTEST_CHECK_MESSAGE(service.errorString().isEmpty(), qPrintable(service.errorString()));
        CTEST_COMPARE(finished.last().at(1).toStringList().size(), 3);
        // Retry reopens the persisted local paths; it does not reuse the
        // original process-local provider callbacks.
        CTEST_COMPARE(reads.load(), 1);
        service.selectJob(jobId);
        CTEST_WAIT_COMPARE_TIMEOUT(service.jobItems().size(), 3, 10000);
        for (const auto &value : service.jobItems()) {
            const auto item = value.toMap();
            CTEST_COMPARE(item.value("state").toString(), QStringLiteral("succeeded"));
            CTEST_COMPARE(item.value("attempts").toULongLong(), qulonglong(2));
        }
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }

    void publicationWaitCanBeCancelledWithoutTouchingTheOwnerOrSource() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-cancel-import-XXXXXX");
        CTEST_CHECK(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        CTEST_CHECK(QDir().mkdir(drivePath)); CTEST_CHECK(SocietyDrive::create(drivePath));
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{1}}, {"data_offsets", QJsonArray{0, 1}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data()); bytes += header + QByteArray("x");
        const auto path = workspace.filePath("model.safetensors");
        QFile source(path); CTEST_CHECK(source.open(QIODevice::WriteOnly)); CTEST_COMPARE(source.write(bytes), bytes.size()); source.close();
        ModelImportService service;
        society_ctest::SignalRecorder finished(&service, &ModelImportService::finished);
        society_ctest::SignalRecorder organized(&service, &ModelImportService::organized);
        service.setContainerPath(drivePath);
        CTEST_WAIT_VERIFY(!organized.isEmpty(), 30000);
        CTEST_CHECK(QDir().mkpath(drivePath + "/.society-sync"));
        QLockFile owner(drivePath + "/.society-sync/operation.lock"); owner.setStaleLockTime(0); CTEST_CHECK(owner.tryLock());
        ModelImportSource input{QStringLiteral("model.safetensors"),
            [path](const auto &consume, const auto &) { consume(path); return QString(); }, path};
        CTEST_CHECK(service.importSources({input}));
        CTEST_WAIT_VERIFY(service.status().contains(QStringLiteral("Waiting for another container operation")), 5000);
        service.cancel();
        CTEST_WAIT_COMPARE_TIMEOUT(finished.size(), 1, 10000);
        CTEST_CHECK(service.status().contains(QStringLiteral("cancelled"), Qt::CaseInsensitive));
        CTEST_CHECK(finished.first().at(1).toStringList().isEmpty());
        CTEST_CHECK(owner.isLocked());
        CTEST_CHECK(QFileInfo::exists(owner.fileName()));
        CTEST_CHECK(source.open(QIODevice::ReadOnly)); CTEST_COMPARE(source.readAll(), bytes);
        CTEST_CHECK(QDir(drivePath + "/Models").entryList({".society-import-*"}, QDir::Files | QDir::Hidden).isEmpty());
        owner.unlock();
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }

    void multipleLargeModelsCrossCopyCheckpointsAndKeepTheirBytes() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-large-import-XXXXXX");
        CTEST_CHECK(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        CTEST_CHECK(QDir().mkdir(drivePath)); CTEST_CHECK(SocietyDrive::create(drivePath));
        constexpr qint64 payloadSize = 65 * 1024 * 1024;
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{payloadSize}}, {"data_offsets", QJsonArray{0, payloadSize}}}}}).toJson(QJsonDocument::Compact);
        QByteArray prefix(8, '\0'); qToLittleEndian(quint64(header.size()), prefix.data()); prefix += header;
        QList<ModelImportSource> sources;
        for (int index = 0; index < 2; ++index) {
            const auto name = QStringLiteral("large%1.safetensors").arg(index);
            const auto path = workspace.filePath(name);
            QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write(prefix), prefix.size());
            CTEST_CHECK(file.resize(prefix.size() + payloadSize)); file.close();
            sources.append({name, [path](const auto &consume, const auto &) { consume(path); return QString(); }, path});
        }
        ModelImportService service;
        society_ctest::SignalRecorder finished(&service, &ModelImportService::finished);
        bool sawSecond = false;
        connect(&service, &ModelImportService::stateChanged, &service, [&] {
            sawSecond |= service.status().contains(QStringLiteral("2 of 2"));
        });
        service.setContainerPath(drivePath);
        CTEST_CHECK(service.importSources(sources));
        CTEST_WAIT_COMPARE_TIMEOUT(finished.size(), 1, 180000);
        CTEST_CHECK_MESSAGE(service.errorString().isEmpty(), qPrintable(service.errorString()));
        const auto paths = finished.first().at(1).toStringList();
        CTEST_COMPARE(paths.size(), 2); CTEST_CHECK(sawSecond); CTEST_COMPARE(service.progress(), 1.0);
        const auto digest = [](const QString &path) {
            QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
            return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result() : QByteArray();
        };
        for (int index = 0; index < paths.size(); ++index) {
            CTEST_COMPARE(QFileInfo(paths[index]).size(), prefix.size() + payloadSize);
            const auto sourceDigest = digest(sources[index].resumablePath);
            CTEST_CHECK(!sourceDigest.isEmpty()); CTEST_COMPARE(digest(paths[index]), sourceDigest);
        }
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }

    void importKeepsMetadataWithTheContainerObject() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-model-object-XXXXXX");
        CTEST_CHECK(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        CTEST_CHECK(QDir().mkdir(drivePath));
        const auto drive = SocietyDrive::create(drivePath); CTEST_CHECK(drive);
        const auto sourceDirectory = workspace.filePath("source");
        CTEST_CHECK(QDir().mkdir(sourceDirectory));
        const auto source = sourceDirectory + "/model.safetensors";
        const auto description = sourceDirectory + "/model.model.json";
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{4}}, {"data_offsets", QJsonArray{0, 4}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data());
        bytes += header + QByteArray("test");
        const auto write = [](const QString &path, const QByteArray &data) {
            QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        CTEST_CHECK(write(source, bytes));
        CTEST_CHECK(write(description, "{\"society.model_type\":\"LLM\"}"));
        iiFileProvider::FileCatalog external(std::filesystem::path(sourceDirectory.toStdU16String()));
        iiFileProvider::FileMetadata modelMetadata; modelMetadata.title = "Creator model";
        modelMetadata.description = "Source annotations"; modelMetadata.keywords = {"concept", "model"};
        modelMetadata.rating = 4; modelMetadata.fields["license"] = "creator-license";
        iiFileProvider::FileMetadata descriptionMetadata; descriptionMetadata.title = "Creator description";
        CTEST_CHECK(external.registerFile("model.safetensors", modelMetadata).metadataWritten);
        CTEST_CHECK(external.registerFile("model.model.json", descriptionMetadata).metadataWritten);
        CTEST_CHECK(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(30)));
        const auto category = drivePath + "/Models/LLM";
        CTEST_CHECK(QDir().mkpath(category));
        CTEST_CHECK(write(category + "/model.safetensors", bytes));
        ModelImportService service;
        society_ctest::SignalRecorder finished(&service, &ModelImportService::finished);
        society_ctest::SignalRecorder organized(&service, &ModelImportService::organized);
        service.setContainerPath(drivePath);
        CTEST_WAIT_VERIFY(!organized.isEmpty(), 120000);
        const auto unrelated = category + "/model (1).safetensors.iisacc.json";
        CTEST_CHECK(write(unrelated, "unrelated metadata"));
        ModelImportSource input{QStringLiteral("model.safetensors"),
            [source](const auto &consume, const auto &) { consume(source); return QString(); }, source};
        CTEST_CHECK(service.importSources({input}));
        CTEST_WAIT_COMPARE_TIMEOUT(finished.size(), 1, 120000);
        CTEST_CHECK_MESSAGE(service.errorString().isEmpty(), qPrintable(service.errorString()));
        const auto paths = finished.first().at(1).toStringList();
        CTEST_COMPARE(paths.size(), 1);
        const auto imported = category + "/model (2).safetensors";
        CTEST_COMPARE(paths.first(), imported);
        const auto importedDescription = category + "/model (2).model.json";
        CTEST_CHECK(QFileInfo::exists(importedDescription));
        iiFileProvider::FileCatalog catalog(std::filesystem::path((drivePath + "/Models").toStdU16String()));
        const auto modelRecord = catalog.lookupPath("LLM/model (2).safetensors"); CTEST_CHECK(modelRecord);
        CTEST_COMPARE(QString::fromStdString(modelRecord->metadata.title), QStringLiteral("Creator model"));
        CTEST_CHECK(modelRecord->metadata.description == modelMetadata.description);
        CTEST_CHECK(modelRecord->metadata.keywords == modelMetadata.keywords);
        CTEST_COMPARE(modelRecord->metadata.rating, modelMetadata.rating);
        CTEST_CHECK(modelRecord->metadata.fields == modelMetadata.fields);
        const auto descriptionRecord = catalog.lookupPath("LLM/model (2).model.json"); CTEST_CHECK(descriptionRecord);
        CTEST_COMPARE(QString::fromStdString(descriptionRecord->metadata.title), QStringLiteral("Creator description"));
        const auto renamed = FileOperations(*drive).perform(FileOperations::Action::Rename, imported, "renamed.safetensors");
        CTEST_CHECK_MESSAGE(renamed, qPrintable(renamed.error));
        CTEST_CHECK(QFileInfo::exists(classificationPath(drivePath, renamed.path)));
        CTEST_CHECK(QFileInfo::exists(classificationPath(drivePath, category + "/renamed.model.json")));
        const auto trashed = FileOperations(*drive).perform(FileOperations::Action::Trash, renamed.path);
        CTEST_CHECK_MESSAGE(trashed, qPrintable(trashed.error));
        CTEST_CHECK(QFileInfo::exists(classificationPath(drivePath, trashed.path)));
        const auto restored = FileOperations(*drive).perform(FileOperations::Action::Restore, trashed.path);
        CTEST_CHECK_MESSAGE(restored, qPrintable(restored.error));
        CTEST_CHECK(QFileInfo::exists(classificationPath(drivePath, restored.path)));
        CTEST_CHECK(QFileInfo::exists(classificationPath(drivePath, category + "/renamed.model.json")));
        const auto removedFromView = FileOperations(*drive).perform(FileOperations::Action::Trash, restored.path);
        CTEST_CHECK_MESSAGE(removedFromView, qPrintable(removedFromView.error));
        const auto deletedDescription = QFileInfo(removedFromView.path).absolutePath() + "/renamed.model.json";
        CTEST_CHECK(QFileInfo::exists(classificationPath(drivePath, deletedDescription)));
        const auto removed = FileOperations(*drive).perform(FileOperations::Action::Remove, removedFromView.path);
        CTEST_CHECK_MESSAGE(removed, qPrintable(removed.error));
        CTEST_CHECK(!QFileInfo::exists(removedFromView.path));
        CTEST_CHECK(!QFileInfo::exists(classificationPath(drivePath, removedFromView.path)));
        CTEST_CHECK(!QFileInfo::exists(deletedDescription));
        CTEST_CHECK(!QFileInfo::exists(classificationPath(drivePath, deletedDescription)));
        CTEST_CHECK(QFileInfo::exists(source + ".iisacc.json"));
        CTEST_CHECK(QFileInfo::exists(description + ".iisacc.json"));
        QFile untouched(unrelated); CTEST_CHECK(untouched.open(QIODevice::ReadOnly));
        CTEST_COMPARE(untouched.readAll(), QByteArray("unrelated metadata"));
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }
    void invalidPortableMetadataDoesNotPublishTheModel() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-bad-metadata-XXXXXX"); CTEST_CHECK(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer"); CTEST_CHECK(QDir().mkdir(drivePath));
        CTEST_CHECK(SocietyDrive::create(drivePath));
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{1}}, {"data_offsets", QJsonArray{0, 1}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data()); bytes += header + QByteArray("x");
        const auto source = workspace.filePath("broken.safetensors");
        QFile file(source); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write(bytes), bytes.size()); file.close();
        QFile metadata(source + ".iisacc.json"); CTEST_CHECK(metadata.open(QIODevice::WriteOnly));
        CTEST_COMPARE(metadata.write("invalid"), 7); metadata.close();
        ModelImportService service; society_ctest::SignalRecorder finished(&service, &ModelImportService::finished);
        service.setContainerPath(drivePath);
        ModelImportSource input{QStringLiteral("broken.safetensors"),
            [source](const auto &consume, const auto &) { consume(source); return QString(); }, source};
        CTEST_CHECK(service.importSources({input}));
        CTEST_WAIT_COMPARE_TIMEOUT(finished.size(), 1, 120000);
        CTEST_CHECK(!service.errorString().isEmpty());
        CTEST_CHECK(finished.first().at(1).toStringList().isEmpty());
        CTEST_CHECK(!QFileInfo::exists(drivePath + "/Models/Other/broken.safetensors"));
        CTEST_CHECK(metadata.open(QIODevice::ReadOnly)); CTEST_COMPARE(metadata.readAll(), QByteArray("invalid"));
        CTEST_CHECK(file.open(QIODevice::ReadOnly)); CTEST_COMPARE(file.readAll(), bytes);
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }
    void concurrentImportsKeepEachModelsOwnMetadata() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-concurrent-objects-XXXXXX"); CTEST_CHECK(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer"); CTEST_CHECK(QDir().mkdir(drivePath));
        CTEST_CHECK(SocietyDrive::create(drivePath));
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{1}}, {"data_offsets", QJsonArray{0, 1}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data()); bytes += header + QByteArray("x");
        QList<QString> sources;
        for (const auto &label : {QStringLiteral("first"), QStringLiteral("second")}) {
            const auto directory = workspace.filePath(label); CTEST_CHECK(QDir().mkdir(directory));
            const auto path = directory + "/same.safetensors";
            QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write(bytes), bytes.size()); file.close();
            iiFileProvider::FileCatalog catalog(std::filesystem::path(directory.toStdU16String()));
            iiFileProvider::FileMetadata metadata; metadata.title = label.toStdString();
            CTEST_CHECK(catalog.registerFile("same.safetensors", metadata).metadataWritten);
            sources.append(path);
        }
        CTEST_CHECK(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(30)));
        ModelImportService first, second;
        society_ctest::SignalRecorder firstDone(&first, &ModelImportService::finished); society_ctest::SignalRecorder secondDone(&second, &ModelImportService::finished);
        first.setContainerPath(drivePath); second.setContainerPath(drivePath);
        const auto input = [](const QString &path) {
            return ModelImportSource{QStringLiteral("same.safetensors"),
                [path](const auto &consume, const auto &) { consume(path); return QString(); }, path};
        };
        CTEST_CHECK(first.importSources({input(sources[0])}));
        CTEST_CHECK(second.importSources({input(sources[1])}));
        CTEST_WAIT_COMPARE_TIMEOUT(firstDone.size(), 1, 120000);
        CTEST_WAIT_COMPARE_TIMEOUT(secondDone.size(), 1, 120000);
        CTEST_CHECK_MESSAGE(first.errorString().isEmpty(), qPrintable(first.errorString()));
        CTEST_CHECK_MESSAGE(second.errorString().isEmpty(), qPrintable(second.errorString()));
        const auto firstPath = firstDone.first().at(1).toStringList().value(0);
        const auto secondPath = secondDone.first().at(1).toStringList().value(0);
        CTEST_CHECK(!firstPath.isEmpty() && !secondPath.isEmpty() && firstPath != secondPath);
        const auto models = drivePath + "/Models";
        iiFileProvider::FileCatalog catalog(std::filesystem::path(models.toStdU16String()));
        const auto firstRecord = catalog.lookupPath(QDir(models).relativeFilePath(firstPath).toStdString());
        const auto secondRecord = catalog.lookupPath(QDir(models).relativeFilePath(secondPath).toStdString());
        CTEST_CHECK(firstRecord && secondRecord);
        CTEST_COMPARE(QString::fromStdString(firstRecord->metadata.title), QStringLiteral("first"));
        CTEST_COMPARE(QString::fromStdString(secondRecord->metadata.title), QStringLiteral("second"));
        const auto storage = SharedStorage::open(drivePath); CTEST_CHECK(storage);
        const auto firstId = storage->fileTracking(StoreSection::Models, QDir(models).relativeFilePath(firstPath)).value("resource").toObject().value("id").toString();
        const auto secondId = storage->fileTracking(StoreSection::Models, QDir(models).relativeFilePath(secondPath)).value("resource").toObject().value("id").toString();
        CTEST_CHECK(!firstId.isEmpty() && !secondId.isEmpty() && firstId != secondId);
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }
    void backgroundOrganizationDoesNotEraseAUserError() {
        QTemporaryDir drive(QDir::tempPath() + "/container-model-errors-XXXXXX.societycontainer");
        CTEST_CHECK(drive.isValid());
        CTEST_CHECK(SocietyDrive::create(drive.path()));
        qputenv("SOCIETY_JOB_STATE_ROOT", drive.filePath(".test-jobs").toUtf8());
        {
            ModelImportService service;
            society_ctest::SignalRecorder organized(&service, &ModelImportService::organized);
            service.setContainerPath(drive.path());
            ModelImportSource invalid{QStringLiteral("../escape.safetensors"),
                [](const auto &, const auto &) { return QString(); }, {}};
            CTEST_CHECK(!service.importSources({invalid}));
            const auto error = service.errorString();
            CTEST_CHECK(!error.isEmpty());
            CTEST_WAIT_COMPARE_TIMEOUT(organized.size(), 1, 120000);
            CTEST_COMPARE(service.errorString(), error);
        }
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }
    void importsAndRecoversJobHistoryWithoutAnApplicationAdapter() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-model-service-XXXXXX");
        CTEST_CHECK(workspace.isValid());
        QCoreApplication::setApplicationName(QStringLiteral("Fixture Studio"));
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        CTEST_CHECK(QDir().mkdir(drivePath));
        QString error;
        const auto drive = SocietyDrive::create(drivePath, &error);
        CTEST_CHECK_MESSAGE(drive.has_value(), qPrintable(error));
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{4}}, {"data_offsets", QJsonArray{0, 4}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data());
        bytes += header + QByteArray("test");
        const auto source = workspace.filePath("model.safetensors");
        QFile file(source); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write(bytes), bytes.size()); file.close();
        QString jobId;
        {
            ModelImportService service;
            society_ctest::SignalRecorder finished(&service, &ModelImportService::finished);
            service.setContainerPath(drivePath);
            ModelImportSource input{QStringLiteral("model.safetensors"),
                [source](const auto &consume, const auto &) { consume(source); return QString(); }, source};
            CTEST_CHECK(service.importSources({input}));
            CTEST_WAIT_COMPARE_TIMEOUT(finished.size(), 1, 120000);
            CTEST_CHECK_MESSAGE(service.errorString().isEmpty(), qPrintable(service.errorString()));
            const auto paths = finished.first().at(1).toStringList();
            CTEST_COMPARE(paths.size(), 1);
            CTEST_CHECK(QFileInfo::exists(paths.first()));
            CTEST_WAIT_VERIFY(!service.organizing(), 120000);
            const auto userResult = service.status();
            CTEST_CHECK(userResult.contains(QStringLiteral("imported"), Qt::CaseInsensitive));
            CTEST_CHECK(service.organizeModels());
            CTEST_CHECK(service.organizing());
            CTEST_COMPARE(service.status(), userResult);
            const auto storage = SharedStorage::open(drivePath);
            CTEST_CHECK(storage);
            const auto relative = QDir(drivePath + "/Models").relativeFilePath(paths.first());
            const auto tracking = storage->fileTracking(StoreSection::Models, relative);
            CTEST_COMPARE(tracking.value("resourceStatus").toString(), QStringLiteral("working-copy-only"));
            CTEST_CHECK(!tracking.value("resource").toObject().value("id").toString().isEmpty());
            CTEST_CHECK(tracking.value("application").toString().isEmpty());
            jobId = service.currentJobId();
            CTEST_CHECK(!jobId.isEmpty());
        }
        {
            ModelImportService service;
            service.setContainerPath(drivePath);
            service.selectJob(jobId);
            CTEST_WAIT_VERIFY(!service.jobs().isEmpty() && !service.jobItems().isEmpty(), 120000);
            CTEST_COMPARE(service.jobItems().first().toMap().value("state").toString(), QStringLiteral("succeeded"));
        }
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    society_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    ModelImportServiceTests tests;
    const auto scoped = [&](auto&& action) {
        const auto previous = qgetenv("SOCIETY_JOB_STATE_ROOT");
        const auto restore = qScopeGuard([&] { previous.isNull() ? qunsetenv("SOCIETY_JOB_STATE_ROOT") : qputenv("SOCIETY_JOB_STATE_ROOT", previous); });
        struct FlushDeferredDeletes {
            ~FlushDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
        } cleanup;
        action();
    };
    runner.run("repairsMissingCategoriesWhenAContainerIsSelected", [&] { scoped([&] { tests.repairsMissingCategoriesWhenAContainerIsSelected(); }); });
    runner.run("catalogOnlyModelSearchAndIdentityConflictsNeedNoObjectStore", [&] { scoped([&] { tests.catalogOnlyModelSearchAndIdentityConflictsNeedNoObjectStore(); }); });
    runner.run("importsDirectlyWithoutWaitingForOrDuplicatingTheObjectStore", [&] { scoped([&] { tests.importsDirectlyWithoutWaitingForOrDuplicatingTheObjectStore(); }); });
    runner.run("staleEmptyOperationLockDoesNotBlockMultipleImports", [&] { scoped([&] { tests.staleEmptyOperationLockDoesNotBlockMultipleImports(); }); });
    runner.run("liveOperationLockIsNotRemovedAndWaitingIsVisible:identified owner", [&] { scoped([&] { tests.liveOperationLockIsNotRemovedAndWaitingIsVisible(false); }); });
#if !defined(Q_OS_WIN) && !defined(Q_OS_ANDROID)
    runner.run("liveOperationLockIsNotRemovedAndWaitingIsVisible:empty but natively locked", [&] { scoped([&] { tests.liveOperationLockIsNotRemovedAndWaitingIsVisible(true); }); });
#endif
    runner.run("publicationTimeoutPreservesRetryableBatchWithoutCopyingTheRest", [&] { scoped([&] { tests.publicationTimeoutPreservesRetryableBatchWithoutCopyingTheRest(); }); });
    runner.run("publicationWaitCanBeCancelledWithoutTouchingTheOwnerOrSource", [&] { scoped([&] { tests.publicationWaitCanBeCancelledWithoutTouchingTheOwnerOrSource(); }); });
    runner.run("multipleLargeModelsCrossCopyCheckpointsAndKeepTheirBytes", [&] { scoped([&] { tests.multipleLargeModelsCrossCopyCheckpointsAndKeepTheirBytes(); }); });
    runner.run("importKeepsMetadataWithTheContainerObject", [&] { scoped([&] { tests.importKeepsMetadataWithTheContainerObject(); }); });
    runner.run("invalidPortableMetadataDoesNotPublishTheModel", [&] { scoped([&] { tests.invalidPortableMetadataDoesNotPublishTheModel(); }); });
    runner.run("concurrentImportsKeepEachModelsOwnMetadata", [&] { scoped([&] { tests.concurrentImportsKeepEachModelsOwnMetadata(); }); });
    runner.run("backgroundOrganizationDoesNotEraseAUserError", [&] { scoped([&] { tests.backgroundOrganizationDoesNotEraseAUserError(); }); });
    runner.run("importsAndRecoversJobHistoryWithoutAnApplicationAdapter", [&] { scoped([&] { tests.importsAndRecoversJobHistoryWithoutAnApplicationAdapter(); }); });
    return runner.result();
}
