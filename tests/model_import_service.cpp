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
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

using namespace iiSocietyContainer;

class ModelImportServiceTests final : public QObject {
    Q_OBJECT
private slots:
    void catalogOnlyModelSearchAndIdentityConflictsNeedNoObjectStore() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-model-catalog-XXXXXX");
        QVERIFY(workspace.isValid());
        const auto root = workspace.filePath("drive.societycontainer");
        QVERIFY(QDir().mkdir(root)); const auto drive = SocietyDrive::create(root); QVERIFY(drive);
        QVERIFY(QDir().mkpath(root + "/Models/Other"));
        const auto path = root + "/Models/Other/creative.safetensors";
        const auto replacement = workspace.filePath("replacement");
        QFile original(path); QVERIFY(original.open(QIODevice::WriteOnly)); QCOMPARE(original.write("original"), qint64(8)); original.close();
        QFile other(replacement); QVERIFY(other.open(QIODevice::WriteOnly)); QCOMPARE(other.write("different"), qint64(9)); other.close();
        const auto store = ModelStore::open(root); QVERIFY(store);
        const auto storage = SharedStorage::open(root); QVERIFY(storage);
        QString error;
        const auto id = store->indexAsset(path, &error); QVERIFY2(!id.isEmpty(), qPrintable(error));
        const auto identifier = drive->storageKind() == SocietyDrive::StorageKind::LogicalDisk
            ? drive->storageIdentifier() : drive->identifier();
        const auto package = FileRepository::packagePath(std::filesystem::path(root.toStdU16String()), identifier.toStdString());
        QVERIFY(!std::filesystem::exists(package / "objects.sqlite3"));
        const auto tracking = storage->fileTracking(StoreSection::Models, "Other/creative.safetensors", &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(tracking.value("resource").toObject().value("id").toString(), id);
        const auto results = storage->searchFiles({{"section", "models"}, {"text", "creative"}}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(results.value("files").toArray().size(), 1);
        QCOMPARE(results.value("files").toArray().first().toObject().value("resourceId").toString(), id);
        const auto unicodeRelative = QString::fromUtf8("Other/모델.safetensors");
        const auto unicodePath = root + "/Models/" + unicodeRelative;
        QFile unicode(unicodePath); QVERIFY(unicode.open(QIODevice::WriteOnly)); QCOMPARE(unicode.write("weights"), qint64(7)); unicode.close();
        const auto unicodeId = store->indexAsset(unicodePath, &error); QVERIFY2(!unicodeId.isEmpty(), qPrintable(error));
        const auto unicodeTracking = storage->fileTracking(StoreSection::Models, unicodeRelative, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(unicodeTracking.value("resource").toObject().value("id").toString(), unicodeId);
#ifdef Q_OS_MACOS
        const auto decomposed = storage->fileTracking(StoreSection::Models, unicodeRelative.normalized(QString::NormalizationForm_D), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(decomposed.value("resource").toObject().value("id").toString(), unicodeId);
#endif
        QVERIFY(QFile::remove(path)); QVERIFY(QFile::rename(replacement, path));
        QVERIFY(store->indexAsset(path, &error).isEmpty());
        QVERIFY(error.contains("different file", Qt::CaseInsensitive));
        QCOMPARE(storage->fileTracking(StoreSection::Models, "Other/creative.safetensors").value("resourceStatus").toString(), QStringLiteral("stale"));
        QFile untouched(path); QVERIFY(untouched.open(QIODevice::ReadOnly)); QCOMPARE(untouched.readAll(), QByteArray("different"));
        QVERIFY(!std::filesystem::exists(package / "objects.sqlite3"));
    }

    void importsDirectlyWithoutWaitingForOrDuplicatingTheObjectStore() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-direct-models-XXXXXX");
        QVERIFY(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        QVERIFY(QDir().mkdir(drivePath));
        const auto drive = SocietyDrive::create(drivePath); QVERIFY(drive);
        const auto storage = SharedStorage::open(drivePath); QVERIFY(storage);
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{4096}}, {"data_offsets", QJsonArray{0, 4096}}}}}).toJson(QJsonDocument::Compact);
        QByteArray data(8, '\0'); qToLittleEndian(quint64(header.size()), data.data()); data += header + QByteArray(4096, 'm');
        QList<ModelImportSource> inputs;
        for (const auto &name : {QStringLiteral("first.safetensors"), QStringLiteral("second.safetensors")}) {
            const auto path = workspace.filePath(name);
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(data), data.size()); file.close();
            inputs.append({name, [path](const auto &consume, const auto &) { consume(path); return QString(); }, path});
        }
        ModelImportService service;
        QSignalSpy organized(&service, &ModelImportService::organized), finished(&service, &ModelImportService::finished);
        service.setContainerPath(drivePath);
        QTRY_VERIFY_WITH_TIMEOUT(!organized.isEmpty(), 30000);
        auto repository = storage->fileRepository();
        const auto identifier = drive->storageKind() == SocietyDrive::StorageKind::LogicalDisk
            ? drive->storageIdentifier() : drive->identifier();
        const auto package = FileRepository::packagePath(std::filesystem::path(drivePath.toStdU16String()), identifier.toStdString());
        std::optional<iiFileProvider::TemporaryLease> occupied;
        occupied.emplace(package / "operations", iiFileProvider::TemporaryLease::Mode::TryExclusive);
        QVERIFY(occupied->locked());
        QVERIFY(service.importSources(inputs));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QVERIFY2(service.errorString().isEmpty(), qPrintable(service.errorString()));
        QVERIFY(occupied->locked());
        const auto paths = finished.first().at(1).toStringList();
        QCOMPARE(paths.size(), 2);
        QStringList ids;
        for (const auto &path : paths) {
            QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), data);
            const auto relative = QDir(drivePath + "/Models").relativeFilePath(path);
            const auto tracking = storage->fileTracking(StoreSection::Models, relative);
            QCOMPARE(tracking.value("resourceStatus").toString(), QStringLiteral("working-copy-only"));
            QVERIFY(tracking.value("objectKey").toString().isEmpty());
            QVERIFY(tracking.value("rawKey").toString().isEmpty());
            const auto id = tracking.value("resource").toObject().value("id").toString();
            QVERIFY(!id.isEmpty()); ids.append(id);
            QVERIFY(QFileInfo::exists(path + ".iisacc.json"));
        }
        QVERIFY(ids[0] != ids[1]);
        occupied.reset(); repository.reset();
        QString error;
        QVERIFY2(storage->reconcileFiles(&error), qPrintable(error));
        auto readOnly = storage->fileRepository({}, FileRepository::Access::ReadOnly);
        for (const auto &path : paths) {
            const auto relative = QDir(drivePath + "/Models").relativeFilePath(path);
            QVERIFY(!readOnly->lookup(storage->managedFilePath(StoreSection::Models, relative)));
        }
        const auto result = storage->searchFiles({{"section", "models"}, {"text", "first"}}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(result.value("files").toArray().size(), 1);
        readOnly.reset();
        const auto renamed = FileOperations(*drive).perform(FileOperations::Action::Rename, paths[0], "renamed.safetensors");
        QVERIFY2(renamed, qPrintable(renamed.error));
        const auto movedTracking = storage->fileTracking(StoreSection::Models,
            QDir(drivePath + "/Models").relativeFilePath(renamed.path));
        QCOMPARE(movedTracking.value("resource").toObject().value("id").toString(), ids[0]);
        const auto trashed = FileOperations(*drive).perform(FileOperations::Action::Trash, renamed.path);
        QVERIFY2(trashed, qPrintable(trashed.error));
        QVERIFY2(storage->reconcileFiles(&error), qPrintable(error));
        auto afterTrash = storage->fileRepository({}, FileRepository::Access::ReadOnly);
        QVERIFY(!afterTrash->lookup(storage->managedFilePath(StoreSection::Deleted,
            QDir(drivePath + "/Deleted").relativeFilePath(trashed.path))));
        afterTrash.reset();
        const auto replacementSource = workspace.filePath("renamed.safetensors");
        QFile replacement(replacementSource); QVERIFY(replacement.open(QIODevice::WriteOnly));
        QCOMPARE(replacement.write(data), data.size()); replacement.close();
        ModelImportSource replacementInput{QStringLiteral("renamed.safetensors"),
            [replacementSource](const auto &consume, const auto &) { consume(replacementSource); return QString(); }, replacementSource};
        QVERIFY(service.importSources({replacementInput}));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 30000);
        QVERIFY2(service.errorString().isEmpty(), qPrintable(service.errorString()));
        const auto newPath = finished.last().at(1).toStringList().value(0);
        QVERIFY(!newPath.isEmpty());
        const auto newTracking = storage->fileTracking(StoreSection::Models,
            QDir(drivePath + "/Models").relativeFilePath(newPath));
        QVERIFY(newTracking.value("resource").toObject().value("id").toString() != ids[0]);
        const auto restored = FileOperations(*drive).perform(FileOperations::Action::Restore, trashed.path);
        QVERIFY2(restored, qPrintable(restored.error));
        const auto restoredTracking = storage->fileTracking(StoreSection::Models,
            QDir(drivePath + "/Models").relativeFilePath(restored.path));
        QCOMPARE(restoredTracking.value("resource").toObject().value("id").toString(), ids[0]);
        QFile restoredFile(restored.path); QVERIFY(restoredFile.open(QIODevice::ReadOnly));
        QCOMPARE(restoredFile.readAll(), data);
        restoredFile.close();
        QSignalSpy recategorized(&service, &ModelImportService::recategorized);
        QVERIFY(service.recategorize(restored.path, QStringLiteral("LLM")));
        QTRY_COMPARE_WITH_TIMEOUT(recategorized.size(), 1, 30000);
        QVERIFY2(service.errorString().isEmpty(), qPrintable(service.errorString()));
        const auto recategorizedPath = recategorized.first().at(2).toString();
        const auto recategorizedTracking = storage->fileTracking(StoreSection::Models,
            QDir(drivePath + "/Models").relativeFilePath(recategorizedPath));
        QCOMPARE(recategorizedTracking.value("resource").toObject().value("id").toString(), ids[0]);
        QVERIFY(QFileInfo::exists(recategorizedPath + ".iisacc.json"));
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }

    void staleEmptyOperationLockDoesNotBlockMultipleImports() {
#ifdef Q_OS_ANDROID
        QSKIP("Android does not recover an ownerless lock without a cross-thread native lock guarantee.");
#endif
        QTemporaryDir workspace(QDir::tempPath() + "/container-stale-import-XXXXXX");
        QVERIFY(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        QVERIFY(QDir().mkdir(drivePath));
        QVERIFY(SocietyDrive::create(drivePath));
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{4}}, {"data_offsets", QJsonArray{0, 4}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data());
        bytes += header + QByteArray("test");
        QList<ModelImportSource> sources;
        for (int index = 0; index < 3; ++index) {
            const auto name = QStringLiteral("model%1.safetensors").arg(index);
            const auto path = workspace.filePath(name);
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); file.close();
            sources.append({name, [path](const auto &consume, const auto &) { consume(path); return QString(); }, path});
        }
        ModelImportService service;
        QSignalSpy finished(&service, &ModelImportService::finished);
        QSignalSpy organized(&service, &ModelImportService::organized);
        bool sawBatchProgress = false;
        connect(&service, &ModelImportService::stateChanged, &service, [&] {
            sawBatchProgress |= service.status().contains(QStringLiteral("1 of 3"));
        });
        service.setContainerPath(drivePath);
        QTRY_VERIFY_WITH_TIMEOUT(!organized.isEmpty(), 30000);
        QVERIFY(QDir().mkpath(drivePath + "/.society-sync"));
        QFile abandoned(drivePath + "/.society-sync/operation.lock");
        QVERIFY(abandoned.open(QIODevice::WriteOnly));
        QVERIFY(abandoned.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-600), QFileDevice::FileModificationTime));
        abandoned.close();
        QVERIFY(service.importSources(sources));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QVERIFY2(service.errorString().isEmpty(), qPrintable(service.errorString()));
        QVERIFY(sawBatchProgress);
        const auto paths = finished.first().at(1).toStringList();
        QCOMPARE(paths.size(), sources.size());
        for (const auto &path : paths) {
            QFile imported(path); QVERIFY(imported.open(QIODevice::ReadOnly)); QCOMPARE(imported.readAll(), bytes);
        }
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }

    void liveOperationLockIsNotRemovedAndWaitingIsVisible_data() {
        QTest::addColumn<bool>("malformed");
        QTest::newRow("identified owner") << false;
#if !defined(Q_OS_WIN) && !defined(Q_OS_ANDROID)
        QTest::newRow("empty but natively locked") << true;
#endif
    }
    void liveOperationLockIsNotRemovedAndWaitingIsVisible() {
        QFETCH(bool, malformed);
        QTemporaryDir workspace(QDir::tempPath() + "/container-active-import-XXXXXX");
        QVERIFY(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        QVERIFY(QDir().mkdir(drivePath)); QVERIFY(SocietyDrive::create(drivePath));
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{1}}, {"data_offsets", QJsonArray{0, 1}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data()); bytes += header + QByteArray("x");
        const auto path = workspace.filePath("model.safetensors");
        QFile source(path); QVERIFY(source.open(QIODevice::WriteOnly)); QCOMPARE(source.write(bytes), bytes.size()); source.close();
        ModelImportService service;
        QSignalSpy finished(&service, &ModelImportService::finished);
        QSignalSpy organized(&service, &ModelImportService::organized);
        bool waiting = false;
        connect(&service, &ModelImportService::stateChanged, &service, [&] {
            waiting |= service.status().contains(QStringLiteral("Waiting for another container operation"));
        });
        service.setContainerPath(drivePath);
        QTRY_VERIFY_WITH_TIMEOUT(!organized.isEmpty(), 30000);
        QVERIFY(QDir().mkpath(drivePath + "/.society-sync"));
        QLockFile owner(drivePath + "/.society-sync/operation.lock"); owner.setStaleLockTime(0); QVERIFY(owner.tryLock());
        if (malformed) {
            QFile lockContents(owner.fileName()); QVERIFY(lockContents.open(QIODevice::ReadWrite));
            QVERIFY(lockContents.resize(0));
            QVERIFY(lockContents.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-600), QFileDevice::FileModificationTime));
            lockContents.close();
        }
        ModelImportSource input{QStringLiteral("model.safetensors"),
            [path](const auto &consume, const auto &) { consume(path); return QString(); }, path};
        QVERIFY(service.importSources({input}));
        QTRY_VERIFY_WITH_TIMEOUT(waiting, 5000);
        QTest::qWait(1000);
        QVERIFY(finished.isEmpty());
        QVERIFY(owner.isLocked());
        QVERIFY(QFileInfo::exists(drivePath + "/.society-sync/operation.lock"));
        if (malformed) QCOMPARE(QFileInfo(owner.fileName()).size(), qint64(0));
        owner.unlock();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 30000);
        QVERIFY2(service.errorString().isEmpty(), qPrintable(service.errorString()));
        QCOMPARE(finished.first().at(1).toStringList().size(), 1);
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }

    void publicationTimeoutPreservesRetryableBatchWithoutCopyingTheRest() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-blocked-import-XXXXXX");
        QVERIFY(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        QVERIFY(QDir().mkdir(drivePath)); QVERIFY(SocietyDrive::create(drivePath));
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{1}}, {"data_offsets", QJsonArray{0, 1}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data()); bytes += header + QByteArray("x");
        std::atomic_int reads{0};
        QList<ModelImportSource> sources;
        for (int index = 0; index < 3; ++index) {
            const auto name = QStringLiteral("model%1.safetensors").arg(index);
            const auto path = workspace.filePath(name);
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); file.close();
            sources.append({name, [path, &reads](const auto &consume, const auto &) {
                ++reads; consume(path); return QString();
            }, path});
        }
        ModelImportService service;
        QSignalSpy finished(&service, &ModelImportService::finished);
        QSignalSpy organized(&service, &ModelImportService::organized);
        service.setContainerPath(drivePath);
        QTRY_VERIFY_WITH_TIMEOUT(!organized.isEmpty(), 30000);
        QVERIFY(QDir().mkpath(drivePath + "/.society-sync"));
        QLockFile owner(drivePath + "/.society-sync/operation.lock"); owner.setStaleLockTime(0); QVERIFY(owner.tryLock());
        QVERIFY(service.importSources(sources));
        const auto jobId = service.currentJobId();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 45000);
        QCOMPARE(reads.load(), 1);
        QVERIFY(service.errorString().contains(QStringLiteral("publication lock")));
        QVERIFY(finished.first().at(1).toStringList().isEmpty());
        QVERIFY(owner.isLocked()); owner.unlock();
        service.selectJob(jobId);
        QTRY_COMPARE_WITH_TIMEOUT(service.jobItems().size(), 3, 10000);
        for (const auto &value : service.jobItems()) {
            const auto item = value.toMap();
            QCOMPARE(item.value("state").toString(), QStringLiteral("failed"));
            QVERIFY(item.value("retryable").toBool());
        }
        QVERIFY(service.retryFailed(jobId));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 30000);
        QVERIFY2(service.errorString().isEmpty(), qPrintable(service.errorString()));
        QCOMPARE(finished.last().at(1).toStringList().size(), 3);
        // Retry reopens the persisted local paths; it does not reuse the
        // original process-local provider callbacks.
        QCOMPARE(reads.load(), 1);
        service.selectJob(jobId);
        QTRY_COMPARE_WITH_TIMEOUT(service.jobItems().size(), 3, 10000);
        for (const auto &value : service.jobItems()) {
            const auto item = value.toMap();
            QCOMPARE(item.value("state").toString(), QStringLiteral("succeeded"));
            QCOMPARE(item.value("attempts").toULongLong(), qulonglong(2));
        }
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }

    void publicationWaitCanBeCancelledWithoutTouchingTheOwnerOrSource() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-cancel-import-XXXXXX");
        QVERIFY(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        QVERIFY(QDir().mkdir(drivePath)); QVERIFY(SocietyDrive::create(drivePath));
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{1}}, {"data_offsets", QJsonArray{0, 1}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data()); bytes += header + QByteArray("x");
        const auto path = workspace.filePath("model.safetensors");
        QFile source(path); QVERIFY(source.open(QIODevice::WriteOnly)); QCOMPARE(source.write(bytes), bytes.size()); source.close();
        ModelImportService service;
        QSignalSpy finished(&service, &ModelImportService::finished);
        QSignalSpy organized(&service, &ModelImportService::organized);
        service.setContainerPath(drivePath);
        QTRY_VERIFY_WITH_TIMEOUT(!organized.isEmpty(), 30000);
        QVERIFY(QDir().mkpath(drivePath + "/.society-sync"));
        QLockFile owner(drivePath + "/.society-sync/operation.lock"); owner.setStaleLockTime(0); QVERIFY(owner.tryLock());
        ModelImportSource input{QStringLiteral("model.safetensors"),
            [path](const auto &consume, const auto &) { consume(path); return QString(); }, path};
        QVERIFY(service.importSources({input}));
        QTRY_VERIFY_WITH_TIMEOUT(service.status().contains(QStringLiteral("Waiting for another container operation")), 5000);
        service.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY(service.status().contains(QStringLiteral("cancelled"), Qt::CaseInsensitive));
        QVERIFY(finished.first().at(1).toStringList().isEmpty());
        QVERIFY(owner.isLocked());
        QVERIFY(QFileInfo::exists(owner.fileName()));
        QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), bytes);
        QVERIFY(QDir(drivePath + "/Models").entryList({".society-import-*"}, QDir::Files | QDir::Hidden).isEmpty());
        owner.unlock();
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }

    void multipleLargeModelsCrossCopyCheckpointsAndKeepTheirBytes() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-large-import-XXXXXX");
        QVERIFY(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        QVERIFY(QDir().mkdir(drivePath)); QVERIFY(SocietyDrive::create(drivePath));
        constexpr qint64 payloadSize = 65 * 1024 * 1024;
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{payloadSize}}, {"data_offsets", QJsonArray{0, payloadSize}}}}}).toJson(QJsonDocument::Compact);
        QByteArray prefix(8, '\0'); qToLittleEndian(quint64(header.size()), prefix.data()); prefix += header;
        QList<ModelImportSource> sources;
        for (int index = 0; index < 2; ++index) {
            const auto name = QStringLiteral("large%1.safetensors").arg(index);
            const auto path = workspace.filePath(name);
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(prefix), prefix.size());
            QVERIFY(file.resize(prefix.size() + payloadSize)); file.close();
            sources.append({name, [path](const auto &consume, const auto &) { consume(path); return QString(); }, path});
        }
        ModelImportService service;
        QSignalSpy finished(&service, &ModelImportService::finished);
        bool sawSecond = false;
        connect(&service, &ModelImportService::stateChanged, &service, [&] {
            sawSecond |= service.status().contains(QStringLiteral("2 of 2"));
        });
        service.setContainerPath(drivePath);
        QVERIFY(service.importSources(sources));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 180000);
        QVERIFY2(service.errorString().isEmpty(), qPrintable(service.errorString()));
        const auto paths = finished.first().at(1).toStringList();
        QCOMPARE(paths.size(), 2); QVERIFY(sawSecond); QCOMPARE(service.progress(), 1.0);
        const auto digest = [](const QString &path) {
            QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
            return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result() : QByteArray();
        };
        for (int index = 0; index < paths.size(); ++index) {
            QCOMPARE(QFileInfo(paths[index]).size(), prefix.size() + payloadSize);
            const auto sourceDigest = digest(sources[index].resumablePath);
            QVERIFY(!sourceDigest.isEmpty()); QCOMPARE(digest(paths[index]), sourceDigest);
        }
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }

    void importKeepsMetadataWithTheContainerObject() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-model-object-XXXXXX");
        QVERIFY(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        QVERIFY(QDir().mkdir(drivePath));
        const auto drive = SocietyDrive::create(drivePath); QVERIFY(drive);
        const auto sourceDirectory = workspace.filePath("source");
        QVERIFY(QDir().mkdir(sourceDirectory));
        const auto source = sourceDirectory + "/model.safetensors";
        const auto description = sourceDirectory + "/model.model.json";
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{4}}, {"data_offsets", QJsonArray{0, 4}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data());
        bytes += header + QByteArray("test");
        const auto write = [](const QString &path, const QByteArray &data) {
            QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        QVERIFY(write(source, bytes));
        QVERIFY(write(description, "{\"society.model_type\":\"LLM\"}"));
        iiFileProvider::FileCatalog external(std::filesystem::path(sourceDirectory.toStdU16String()));
        iiFileProvider::FileMetadata modelMetadata; modelMetadata.title = "Creator model";
        modelMetadata.description = "Source annotations"; modelMetadata.keywords = {"concept", "model"};
        modelMetadata.rating = 4; modelMetadata.fields["license"] = "creator-license";
        iiFileProvider::FileMetadata descriptionMetadata; descriptionMetadata.title = "Creator description";
        QVERIFY(external.registerFile("model.safetensors", modelMetadata).metadataWritten);
        QVERIFY(external.registerFile("model.model.json", descriptionMetadata).metadataWritten);
        QVERIFY(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(30)));
        const auto category = drivePath + "/Models/LLM";
        QVERIFY(QDir().mkpath(category));
        QVERIFY(write(category + "/model.safetensors", bytes));
        ModelImportService service;
        QSignalSpy finished(&service, &ModelImportService::finished);
        QSignalSpy organized(&service, &ModelImportService::organized);
        service.setContainerPath(drivePath);
        QTRY_VERIFY_WITH_TIMEOUT(!organized.isEmpty(), 120000);
        const auto unrelated = category + "/model (1).safetensors.iisacc.json";
        QVERIFY(write(unrelated, "unrelated metadata"));
        ModelImportSource input{QStringLiteral("model.safetensors"),
            [source](const auto &consume, const auto &) { consume(source); return QString(); }, source};
        QVERIFY(service.importSources({input}));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 120000);
        QVERIFY2(service.errorString().isEmpty(), qPrintable(service.errorString()));
        const auto paths = finished.first().at(1).toStringList();
        QCOMPARE(paths.size(), 1);
        const auto imported = category + "/model (2).safetensors";
        QCOMPARE(paths.first(), imported);
        const auto importedDescription = category + "/model (2).model.json";
        QVERIFY(QFileInfo::exists(importedDescription));
        iiFileProvider::FileCatalog catalog(std::filesystem::path((drivePath + "/Models").toStdU16String()));
        const auto modelRecord = catalog.lookupPath("LLM/model (2).safetensors"); QVERIFY(modelRecord);
        QCOMPARE(QString::fromStdString(modelRecord->metadata.title), QStringLiteral("Creator model"));
        QVERIFY(modelRecord->metadata.description == modelMetadata.description);
        QVERIFY(modelRecord->metadata.keywords == modelMetadata.keywords);
        QCOMPARE(modelRecord->metadata.rating, modelMetadata.rating);
        QVERIFY(modelRecord->metadata.fields == modelMetadata.fields);
        const auto descriptionRecord = catalog.lookupPath("LLM/model (2).model.json"); QVERIFY(descriptionRecord);
        QCOMPARE(QString::fromStdString(descriptionRecord->metadata.title), QStringLiteral("Creator description"));
        const auto renamed = FileOperations(*drive).perform(FileOperations::Action::Rename, imported, "renamed.safetensors");
        QVERIFY2(renamed, qPrintable(renamed.error));
        QVERIFY(QFileInfo::exists(renamed.path + ".iisacc.json"));
        QVERIFY(QFileInfo::exists(category + "/renamed.model.json.iisacc.json"));
        const auto trashed = FileOperations(*drive).perform(FileOperations::Action::Trash, renamed.path);
        QVERIFY2(trashed, qPrintable(trashed.error));
        QVERIFY(QFileInfo::exists(trashed.path + ".iisacc.json"));
        const auto restored = FileOperations(*drive).perform(FileOperations::Action::Restore, trashed.path);
        QVERIFY2(restored, qPrintable(restored.error));
        QVERIFY(QFileInfo::exists(restored.path + ".iisacc.json"));
        QVERIFY(QFileInfo::exists(category + "/renamed.model.json.iisacc.json"));
        const auto removedFromView = FileOperations(*drive).perform(FileOperations::Action::Trash, restored.path);
        QVERIFY2(removedFromView, qPrintable(removedFromView.error));
        const auto deletedDescription = QFileInfo(removedFromView.path).absolutePath() + "/renamed.model.json";
        QVERIFY(QFileInfo::exists(deletedDescription + ".iisacc.json"));
        const auto removed = FileOperations(*drive).perform(FileOperations::Action::Remove, removedFromView.path);
        QVERIFY2(removed, qPrintable(removed.error));
        QVERIFY(!QFileInfo::exists(removedFromView.path));
        QVERIFY(!QFileInfo::exists(removedFromView.path + ".iisacc.json"));
        QVERIFY(!QFileInfo::exists(deletedDescription));
        QVERIFY(!QFileInfo::exists(deletedDescription + ".iisacc.json"));
        QVERIFY(QFileInfo::exists(source + ".iisacc.json"));
        QVERIFY(QFileInfo::exists(description + ".iisacc.json"));
        QFile untouched(unrelated); QVERIFY(untouched.open(QIODevice::ReadOnly));
        QCOMPARE(untouched.readAll(), QByteArray("unrelated metadata"));
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }
    void invalidPortableMetadataDoesNotPublishTheModel() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-bad-metadata-XXXXXX"); QVERIFY(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer"); QVERIFY(QDir().mkdir(drivePath));
        QVERIFY(SocietyDrive::create(drivePath));
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{1}}, {"data_offsets", QJsonArray{0, 1}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data()); bytes += header + QByteArray("x");
        const auto source = workspace.filePath("broken.safetensors");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); file.close();
        QFile metadata(source + ".iisacc.json"); QVERIFY(metadata.open(QIODevice::WriteOnly));
        QCOMPARE(metadata.write("invalid"), 7); metadata.close();
        ModelImportService service; QSignalSpy finished(&service, &ModelImportService::finished);
        service.setContainerPath(drivePath);
        ModelImportSource input{QStringLiteral("broken.safetensors"),
            [source](const auto &consume, const auto &) { consume(source); return QString(); }, source};
        QVERIFY(service.importSources({input}));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 120000);
        QVERIFY(!service.errorString().isEmpty());
        QVERIFY(finished.first().at(1).toStringList().isEmpty());
        QVERIFY(!QFileInfo::exists(drivePath + "/Models/Other/broken.safetensors"));
        QVERIFY(metadata.open(QIODevice::ReadOnly)); QCOMPARE(metadata.readAll(), QByteArray("invalid"));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes);
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }
    void concurrentImportsKeepEachModelsOwnMetadata() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-concurrent-objects-XXXXXX"); QVERIFY(workspace.isValid());
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer"); QVERIFY(QDir().mkdir(drivePath));
        QVERIFY(SocietyDrive::create(drivePath));
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{1}}, {"data_offsets", QJsonArray{0, 1}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data()); bytes += header + QByteArray("x");
        QList<QString> sources;
        for (const auto &label : {QStringLiteral("first"), QStringLiteral("second")}) {
            const auto directory = workspace.filePath(label); QVERIFY(QDir().mkdir(directory));
            const auto path = directory + "/same.safetensors";
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); file.close();
            iiFileProvider::FileCatalog catalog(std::filesystem::path(directory.toStdU16String()));
            iiFileProvider::FileMetadata metadata; metadata.title = label.toStdString();
            QVERIFY(catalog.registerFile("same.safetensors", metadata).metadataWritten);
            sources.append(path);
        }
        QVERIFY(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(30)));
        ModelImportService first, second;
        QSignalSpy firstDone(&first, &ModelImportService::finished), secondDone(&second, &ModelImportService::finished);
        first.setContainerPath(drivePath); second.setContainerPath(drivePath);
        const auto input = [](const QString &path) {
            return ModelImportSource{QStringLiteral("same.safetensors"),
                [path](const auto &consume, const auto &) { consume(path); return QString(); }, path};
        };
        QVERIFY(first.importSources({input(sources[0])}));
        QVERIFY(second.importSources({input(sources[1])}));
        QTRY_COMPARE_WITH_TIMEOUT(firstDone.size(), 1, 120000);
        QTRY_COMPARE_WITH_TIMEOUT(secondDone.size(), 1, 120000);
        QVERIFY2(first.errorString().isEmpty(), qPrintable(first.errorString()));
        QVERIFY2(second.errorString().isEmpty(), qPrintable(second.errorString()));
        const auto firstPath = firstDone.first().at(1).toStringList().value(0);
        const auto secondPath = secondDone.first().at(1).toStringList().value(0);
        QVERIFY(!firstPath.isEmpty() && !secondPath.isEmpty() && firstPath != secondPath);
        const auto models = drivePath + "/Models";
        iiFileProvider::FileCatalog catalog(std::filesystem::path(models.toStdU16String()));
        const auto firstRecord = catalog.lookupPath(QDir(models).relativeFilePath(firstPath).toStdString());
        const auto secondRecord = catalog.lookupPath(QDir(models).relativeFilePath(secondPath).toStdString());
        QVERIFY(firstRecord && secondRecord);
        QCOMPARE(QString::fromStdString(firstRecord->metadata.title), QStringLiteral("first"));
        QCOMPARE(QString::fromStdString(secondRecord->metadata.title), QStringLiteral("second"));
        const auto storage = SharedStorage::open(drivePath); QVERIFY(storage);
        const auto firstId = storage->fileTracking(StoreSection::Models, QDir(models).relativeFilePath(firstPath)).value("resource").toObject().value("id").toString();
        const auto secondId = storage->fileTracking(StoreSection::Models, QDir(models).relativeFilePath(secondPath)).value("resource").toObject().value("id").toString();
        QVERIFY(!firstId.isEmpty() && !secondId.isEmpty() && firstId != secondId);
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }
    void backgroundOrganizationDoesNotEraseAUserError() {
        QTemporaryDir drive(QDir::tempPath() + "/container-model-errors-XXXXXX.societycontainer");
        QVERIFY(drive.isValid());
        QVERIFY(SocietyDrive::create(drive.path()));
        qputenv("SOCIETY_JOB_STATE_ROOT", drive.filePath(".test-jobs").toUtf8());
        {
            ModelImportService service;
            QSignalSpy organized(&service, &ModelImportService::organized);
            service.setContainerPath(drive.path());
            ModelImportSource invalid{QStringLiteral("../escape.safetensors"),
                [](const auto &, const auto &) { return QString(); }, {}};
            QVERIFY(!service.importSources({invalid}));
            const auto error = service.errorString();
            QVERIFY(!error.isEmpty());
            QTRY_COMPARE_WITH_TIMEOUT(organized.size(), 1, 120000);
            QCOMPARE(service.errorString(), error);
        }
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }
    void importsAndRecoversJobHistoryWithoutAnApplicationAdapter() {
        QTemporaryDir workspace(QDir::tempPath() + "/container-model-service-XXXXXX");
        QVERIFY(workspace.isValid());
        QCoreApplication::setApplicationName(QStringLiteral("Fixture Studio"));
        qputenv("SOCIETY_JOB_STATE_ROOT", workspace.filePath("jobs").toUtf8());
        const auto drivePath = workspace.filePath("drive.societycontainer");
        QVERIFY(QDir().mkdir(drivePath));
        QString error;
        const auto drive = SocietyDrive::create(drivePath, &error);
        QVERIFY2(drive.has_value(), qPrintable(error));
        const auto header = QJsonDocument(QJsonObject{{"tensor", QJsonObject{{"dtype", "U8"},
            {"shape", QJsonArray{4}}, {"data_offsets", QJsonArray{0, 4}}}}}).toJson(QJsonDocument::Compact);
        QByteArray bytes(8, '\0'); qToLittleEndian(quint64(header.size()), bytes.data());
        bytes += header + QByteArray("test");
        const auto source = workspace.filePath("model.safetensors");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); file.close();
        QString jobId;
        {
            ModelImportService service;
            QSignalSpy finished(&service, &ModelImportService::finished);
            service.setContainerPath(drivePath);
            ModelImportSource input{QStringLiteral("model.safetensors"),
                [source](const auto &consume, const auto &) { consume(source); return QString(); }, source};
            QVERIFY(service.importSources({input}));
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 120000);
            QVERIFY2(service.errorString().isEmpty(), qPrintable(service.errorString()));
            const auto paths = finished.first().at(1).toStringList();
            QCOMPARE(paths.size(), 1);
            QVERIFY(QFileInfo::exists(paths.first()));
            QTRY_VERIFY_WITH_TIMEOUT(!service.organizing(), 120000);
            const auto userResult = service.status();
            QVERIFY(userResult.contains(QStringLiteral("imported"), Qt::CaseInsensitive));
            QVERIFY(service.organizeModels());
            QVERIFY(service.organizing());
            QCOMPARE(service.status(), userResult);
            const auto storage = SharedStorage::open(drivePath);
            QVERIFY(storage);
            const auto relative = QDir(drivePath + "/Models").relativeFilePath(paths.first());
            const auto tracking = storage->fileTracking(StoreSection::Models, relative);
            QCOMPARE(tracking.value("resourceStatus").toString(), QStringLiteral("working-copy-only"));
            QVERIFY(!tracking.value("resource").toObject().value("id").toString().isEmpty());
            QVERIFY(tracking.value("application").toString().isEmpty());
            jobId = service.currentJobId();
            QVERIFY(!jobId.isEmpty());
        }
        {
            ModelImportService service;
            service.setContainerPath(drivePath);
            service.selectJob(jobId);
            QTRY_VERIFY_WITH_TIMEOUT(!service.jobs().isEmpty() && !service.jobItems().isEmpty(), 120000);
            QCOMPARE(service.jobItems().first().toMap().value("state").toString(), QStringLiteral("succeeded"));
        }
        qunsetenv("SOCIETY_JOB_STATE_ROOT");
    }
};
QTEST_GUILESS_MAIN(ModelImportServiceTests)
#include "model_import_service.moc"
