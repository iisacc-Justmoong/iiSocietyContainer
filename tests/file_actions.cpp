#include "FileActions.h"
#include "ModelStore.h"
#include "SharedStorage.h"
#include "FileRepository.h"
#include <TemporaryLease.h>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QMimeData>
#include <QLockFile>
#include <QSignalSpy>
#include <QSet>
#include <QTemporaryDir>
#include "../src/DeletedOrigins.h"
#include <QTest>
#include <filesystem>
#include <optional>
using namespace iiSocietyContainer;
class FileActionsTests : public QObject {
    Q_OBJECT
private slots:
    void pathCapabilitiesFollowTheCurrentSelection() {
        QTemporaryDir temp(QDir::tempPath() + "/file-access-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        QVERIFY(SocietyDrive::create(temp.path()));
        const auto first = temp.filePath("Files/first.txt"), second = temp.filePath("Deleted/second.txt");
        for (const auto &path : {first, second}) {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.write("asset") > 0);
        }
        FileActions actions;
        actions.setPath(first);
        actions.setPath(second);
        QTRY_VERIFY_WITH_TIMEOUT(actions.inDeleted() && actions.editable(), 15000);
        actions.setPath(QString());
        QVERIFY(!actions.inDeleted()); QVERIFY(!actions.editable());
    }
    void unrelatedDeletesAreAcceptedAndCompleteWhileAnotherItemIsBusy() {
        QTemporaryDir temp(QDir::tempPath() + "/parallel-delete-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        const auto first = temp.filePath("Deleted/first.bin"), second = temp.filePath("Deleted/second.bin");
        for (const auto &path : {first, second}) {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.write("asset") > 0);
        }
        detail::DeletedOrigins held(*drive); QString error;
        QVERIFY2(held.lockPaths({first}, &error, 0), qPrintable(error));
        FileActions actions;
        QSignalSpy finished(&actions, &FileActions::operationFinished);
        actions.setPath(first); actions.remove();
        actions.setPath(second); actions.remove();
        QCOMPARE(actions.activeCount(), 2);
        QVERIFY(actions.busyForPath(first)); QVERIFY(actions.busyForPath(second));
        QElapsedTimer elapsed; elapsed.start();
        while (QFileInfo::exists(second) && elapsed.elapsed() < 1500) QTest::qWait(10);
        const bool secondFinishedWhileFirstBlocked = !QFileInfo::exists(second) && QFileInfo::exists(first);
        held.unlock();
        QVERIFY2(secondFinishedWhileFirstBlocked, "An unrelated removal waited for the first asset's lock");
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 15000);
        QCOMPARE(finished.size(), 2);
        QVERIFY(!QFileInfo::exists(first)); QVERIFY(!QFileInfo::exists(second));
        QSet<QString> sources;
        for (const auto &event : finished) { sources.insert(event.at(0).toString()); QVERIFY(event.at(2).toString().isEmpty()); }
        QCOMPARE(sources, QSet<QString>({first, second}));
    }
    void closingAnAdapterDoesNotCancelAnAcceptedRemoval() {
        QTemporaryDir temp(QDir::tempPath() + "/detached-delete-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        const auto path = temp.filePath("Deleted/pending.txt");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.write("asset") > 0); file.close();
        detail::DeletedOrigins held(*drive); QString error;
        QVERIFY2(held.lockPaths({path}, &error, 0), qPrintable(error));
        auto actions = std::make_unique<FileActions>();
        actions->removePath(path);
        QCOMPARE(actions->activeCount(), 1);
        QElapsedTimer elapsed; elapsed.start(); actions.reset();
        QVERIFY2(elapsed.elapsed() < 500, "Closing the view blocked on the removal worker");
        held.unlock();
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(path), 15000);
    }
    void renamesLocalModelWithoutDownloadingOrReadingPayload() {
        QTemporaryDir temp(QDir::tempPath() + "/rename-model-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        QVERIFY(QDir().mkpath(temp.filePath("Models/Checkpoint")));
        const auto source = temp.filePath("Models/Checkpoint/old.safetensors");
        const auto destination = temp.filePath("Models/Checkpoint/new.safetensors");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly));
        const auto size = 8LL * 1024 * 1024 * 1024;
        QVERIFY(file.resize(size)); file.close();
        StorageMap map(*drive);
        QVERIFY(map.publish({QJsonObject{{"path", "models/Checkpoint/old.safetensors"}, {"kind", "file"},
            {"size", "1"}, {"resident", false}, {"version", QString(64, 'a')}}}));
        FileActions actions;
        QSignalSpy completed(&actions, &FileActions::completed), failed(&actions, &FileActions::failed);
        actions.setPath(source); actions.rename("new.safetensors");
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 30000);
        QVERIFY2(failed.isEmpty(), qPrintable(actions.errorString()));
        QCOMPARE(completed.size(), 1);
        QCOMPARE(completed.first().first().toString(), destination);
        QVERIFY(!QFileInfo::exists(source));
        QCOMPARE(QFileInfo(destination).size(), size);
        QVERIFY(map.pendingRequests().isEmpty());
        const auto storage = SharedStorage::open(temp.path()); QVERIFY(storage);
        const auto tracking = storage->fileTracking(StoreSection::Models, "Checkpoint/new.safetensors");
        // The model catalog can already describe the working copy without a
        // managed content revision or reading the weights into RAW storage.
        QCOMPARE(tracking.value("status").toString(), QStringLiteral("ready"));
        QCOMPARE(tracking.value("resourceStatus").toString(), QStringLiteral("working-copy-only"));
        QCOMPARE(tracking.value("resource").toObject().value("contentRevisionStatus").toString(), QStringLiteral("uncommitted"));
        QVERIFY(!tracking.contains("rawKey"));
        const auto store = ModelStore::open(temp.path()); QVERIFY(store);
        QString error;
        QCOMPARE(store->resolve("Checkpoint/old.safetensors", &error), destination);
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void renameKeepsTrackedIdentityAndDefersChangedContent() {
        QTemporaryDir temp(QDir::tempPath() + "/rename-tracked-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        QVERIFY(SocietyDrive::create(temp.path()));
        QVERIFY(QDir().mkpath(temp.filePath("Models/Checkpoint")));
        const auto source = temp.filePath("Models/Checkpoint/old.safetensors");
        const auto destination = temp.filePath("Models/Checkpoint/new.safetensors");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.write("original weights") > 0); file.close();
        const auto storage = SharedStorage::open(temp.path()); QVERIFY(storage);
        QString error;
        QVERIFY2(storage->recordFile(StoreSection::Models, "Checkpoint/old.safetensors", &error), qPrintable(error));
        const auto before = storage->fileTracking(StoreSection::Models, "Checkpoint/old.safetensors", &error);
        QVERIFY2(!before.isEmpty(), qPrintable(error));
        const auto key = before.value("objectKey");
        const auto revision = before.value("resource").toObject().value("contentRevision");
        QVERIFY(!key.toString().isEmpty()); QVERIFY(!revision.toString().isEmpty());
        QVERIFY(file.open(QIODevice::Append)); QVERIFY(file.write(" changed") > 0); file.close();
        FileActions actions; QSignalSpy completed(&actions, &FileActions::completed), failed(&actions, &FileActions::failed);
        actions.setPath(source); actions.rename("new.safetensors");
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 30000);
        QVERIFY2(failed.isEmpty(), qPrintable(actions.errorString())); QCOMPARE(completed.size(), 1);
        const auto moved = storage->fileTracking(StoreSection::Models, "Checkpoint/new.safetensors", &error);
        QVERIFY2(!moved.isEmpty(), qPrintable(error));
        QCOMPARE(moved.value("objectKey"), key);
        QCOMPARE(moved.value("resource").toObject().value("contentRevision"), revision);
        QVERIFY2(storage->recordFile(StoreSection::Models, "Checkpoint/new.safetensors", &error), qPrintable(error));
        const auto revised = storage->fileTracking(StoreSection::Models, "Checkpoint/new.safetensors", &error);
        QCOMPARE(revised.value("objectKey"), key);
        QVERIFY(revised.value("resource").toObject().value("contentRevision") != revision);
        QVERIFY(!QFileInfo::exists(source)); QVERIFY(QFileInfo::exists(destination));
    }

    void renameChangesOnlyLetterCaseOnCaseInsensitiveStorage() {
        QTemporaryDir temp(QDir::tempPath() + "/rename-case-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        QVERIFY(SocietyDrive::create(temp.path()));
        QVERIFY(QDir().mkpath(temp.filePath("Models/Checkpoint")));
        const auto source = temp.filePath("Models/Checkpoint/CaseModel.safetensors");
        const auto destination = temp.filePath("Models/Checkpoint/casemodel.safetensors");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.write("weights") > 0); file.close();
        if (!QFileInfo::exists(destination)) QSKIP("This filesystem distinguishes filename case.");
        std::error_code ec;
        QVERIFY(std::filesystem::equivalent(std::filesystem::path(source.toStdU16String()),
                                            std::filesystem::path(destination.toStdU16String()), ec));
        QVERIFY(!ec);
        const auto storage = SharedStorage::open(temp.path()); QVERIFY(storage);
        QString error;
        QVERIFY2(storage->recordFile(StoreSection::Models, "Checkpoint/CaseModel.safetensors", &error), qPrintable(error));
        const auto before = storage->fileTracking(StoreSection::Models, "Checkpoint/CaseModel.safetensors").value("objectKey");
        QVERIFY(!before.toString().isEmpty());
        FileActions actions; QSignalSpy completed(&actions, &FileActions::completed), failed(&actions, &FileActions::failed);
        actions.setPath(source); actions.rename("casemodel.safetensors");
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 30000);
        QVERIFY2(failed.isEmpty(), qPrintable(actions.errorString()));
        QCOMPARE(completed.size(), 1); QCOMPARE(completed.first().first().toString(), destination);
        const auto names = QDir(QFileInfo(source).absolutePath()).entryList(QDir::Files | QDir::NoDotAndDotDot);
        QVERIFY(names.contains("casemodel.safetensors", Qt::CaseSensitive));
        QVERIFY(!names.contains("CaseModel.safetensors", Qt::CaseSensitive));
        QVERIFY(names.contains("casemodel.safetensors.iisacc.json", Qt::CaseSensitive));
        for (const auto &name : names) QVERIFY(!name.startsWith("SocietyRename-"));
        QCOMPARE(storage->fileTracking(StoreSection::Models, "Checkpoint/casemodel.safetensors").value("objectKey"), before);
        const auto store = ModelStore::open(temp.path()); QVERIFY(store);
        QCOMPARE(store->resolve("Checkpoint/CaseModel.safetensors", &error), destination);
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void renameMovesModelPackagesAndKeepsTrackedChildren() {
        QTemporaryDir temp(QDir::tempPath() + "/rename-package-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        QVERIFY(SocietyDrive::create(temp.path()));
        const auto source = temp.filePath("Models/LLM/old-package");
        const auto destination = temp.filePath("Models/LLM/new-package");
        QVERIFY(QDir().mkpath(source));
        QFile config(source + "/config.json"); QVERIFY(config.open(QIODevice::WriteOnly));
        QVERIFY(config.write("{}") > 0); config.close();
        QFile weights(source + "/model.bin"); QVERIFY(weights.open(QIODevice::WriteOnly));
        QVERIFY(weights.write("model weights") > 0); weights.close();
        const auto storage = SharedStorage::open(temp.path()); QVERIFY(storage);
        QString error;
        QVERIFY2(storage->recordFile(StoreSection::Models, "LLM/old-package/model.bin", &error), qPrintable(error));
        const auto before = storage->fileTracking(StoreSection::Models, "LLM/old-package/model.bin").value("objectKey");
        QVERIFY(!before.toString().isEmpty());
        FileActions actions; QSignalSpy completed(&actions, &FileActions::completed), failed(&actions, &FileActions::failed);
        actions.setPath(source); actions.rename("new-package");
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 30000);
        QVERIFY2(failed.isEmpty(), qPrintable(actions.errorString()));
        QCOMPARE(completed.size(), 1); QCOMPARE(completed.first().first().toString(), destination);
        QVERIFY(!QFileInfo::exists(source)); QVERIFY(QFileInfo::exists(destination + "/config.json"));
        QVERIFY(QFileInfo::exists(destination + "/model.bin.iisacc.json"));
        QCOMPARE(storage->fileTracking(StoreSection::Models, "LLM/new-package/model.bin").value("objectKey"), before);
        const auto store = ModelStore::open(temp.path()); QVERIFY(store);
        QCOMPARE(store->resolve("LLM/old-package", &error), destination);
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void renameWaitsForTheIndexWithoutMovingTheFileEarly() {
        QTemporaryDir temp(QDir::tempPath() + "/rename-lock-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        QVERIFY(QDir().mkpath(temp.filePath("Models/Checkpoint")));
        const auto source = temp.filePath("Models/Checkpoint/old.safetensors");
        const auto destination = temp.filePath("Models/Checkpoint/new.safetensors");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.write("weights") > 0); file.close();
        const auto storage = SharedStorage::open(temp.path()); QVERIFY(storage);
        QString error;
        QVERIFY2(storage->recordFile(StoreSection::Models, "Checkpoint/old.safetensors", &error), qPrintable(error));
        const auto original = storage->fileTracking(StoreSection::Models, "Checkpoint/old.safetensors").value("objectKey");
        QVERIFY(!original.toString().isEmpty());
        FileActions actions; QSignalSpy completed(&actions, &FileActions::completed), failed(&actions, &FileActions::failed);
        const auto id = drive->storageKind() == SocietyDrive::StorageKind::LogicalDisk ? drive->storageIdentifier() : drive->identifier();
        const auto path = FileRepository::packagePath(std::filesystem::path(temp.path().toStdU16String()), id.toStdString()) / "operations";
        std::optional<iiFileProvider::TemporaryLease> lock;
        lock.emplace(path, iiFileProvider::TemporaryLease::Mode::TryExclusive); QVERIFY(lock->locked());
        actions.setPath(source); actions.rename("new.safetensors");
        QTest::qWait(250);
        const bool waited = actions.busy() && QFileInfo::exists(source) && !QFileInfo::exists(destination);
        lock.reset();
        QVERIFY(waited);
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 30000);
        QVERIFY2(failed.isEmpty(), qPrintable(actions.errorString())); QCOMPARE(completed.size(), 1);
        QCOMPARE(storage->fileTracking(StoreSection::Models, "Checkpoint/new.safetensors").value("objectKey"), original);
    }

    void renameRejectsRemoteOnlyAndReplicaFilesWithoutDownloading() {
        QTemporaryDir temp(QDir::tempPath() + "/rename-replica-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto created = SocietyDrive::create(temp.path()); QVERIFY(created);
        QVERIFY(SocietyDrive::adoptReplicaIdentity(temp.path(), created->identifier(), created->identifier()));
        QVERIFY(SocietyDrive::completeReplica(temp.path(), created->identifier()));
        const auto drive = SocietyDrive::open(temp.path()); QVERIFY(drive);
        QVERIFY(QDir().mkpath(temp.filePath("Models/Checkpoint")));
        const auto path = temp.filePath("Models/Checkpoint/model.safetensors");
        StorageMap map(*drive); QVERIFY(!map.isLocalAuthority());
        QVERIFY(map.publish({QJsonObject{{"path", "models/Checkpoint/model.safetensors"}, {"kind", "file"},
            {"size", "7"}, {"resident", false}, {"version", QString(64, 'a')}}}));
        FileActions actions; QSignalSpy failed(&actions, &FileActions::failed);
        actions.setPath(path); actions.rename("new.safetensors");
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 15000);
        QCOMPARE(failed.size(), 1); QVERIFY(map.pendingRequests().isEmpty());
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("weights"), 7); file.close();
        actions.rename("new.safetensors");
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 15000);
        QCOMPARE(failed.size(), 2); QVERIFY(actions.errorString().contains("host"));
        QVERIFY(QFileInfo::exists(path)); QVERIFY(!QFileInfo::exists(temp.filePath("Models/Checkpoint/new.safetensors")));
        QVERIFY(map.pendingRequests().isEmpty());
    }

    void deletionNeverQueuesDownloads() {
        QTemporaryDir temp(QDir::tempPath() + "/delete-no-pull-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive); StorageMap map(*drive);
        const auto source = temp.filePath("Models/Checkpoint/model.safetensors");
        QVERIFY(QDir().mkdir(temp.filePath("Models/Checkpoint")));
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.resize(8LL * 1024 * 1024 * 1024)); file.close();
        QJsonObject entry{{"path", "models/Checkpoint/model.safetensors"}, {"kind", "file"},
            {"size", "1"}, {"resident", false}, {"version", QString(64, 'a')}};
        QVERIFY(map.publish({entry}));
        QLockFile sync(temp.filePath(".society-sync/operation.lock")); QVERIFY(sync.tryLock());
        FileActions actions; QSignalSpy done(&actions, &FileActions::completed), failed(&actions, &FileActions::failed);
        actions.setPath(source); actions.trash();
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 1500);
        QCOMPARE(done.size(), 1); QVERIFY(failed.isEmpty()); QVERIFY(map.pendingRequests().isEmpty());
        const auto deleted = done.first().first().toString(); QVERIFY(QFileInfo::exists(deleted));
        entry["path"] = "deleted/model.safetensors"; QVERIFY(map.publish({entry}));
        actions.setPath(deleted); actions.remove();
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 1500);
        QCOMPARE(done.size(), 2); QVERIFY(failed.isEmpty()); QVERIFY(map.pendingRequests().isEmpty());
        QVERIFY(!QFileInfo::exists(deleted));
        // Remote-only files cannot be removed locally, and must not be fetched to delete them.
        entry["path"] = "models/remote.safetensors"; QVERIFY(map.publish({entry}));
        actions.setPath(temp.filePath("Models/remote.safetensors")); actions.trash();
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 1500);
        QCOMPARE(failed.size(), 1); QCOMPARE(done.size(), 2); QVERIFY(map.pendingRequests().isEmpty());
    }
    void copiesUrlsAndPastesWithoutOverwriting() {
        QTemporaryDir temp(QDir::tempPath() + "/clipboard-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        QVERIFY(SocietyDrive::create(temp.path()));
        const auto source = temp.filePath("Models/test.safetensors");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("weights"); file.close();
        FileActions actions; QSignalSpy done(&actions, &FileActions::completed); QSignalSpy failed(&actions, &FileActions::failed);
        actions.setPath(source); actions.copy();
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 120000); QVERIFY2(failed.isEmpty(), qPrintable(actions.errorString())); QCOMPARE(done.size(), 1);
        QCOMPARE(QGuiApplication::clipboard()->mimeData()->urls(), QList<QUrl>{QUrl::fromLocalFile(source)});
        QVERIFY(actions.canPaste());
        actions.paste(temp.filePath("Models")); QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 120000);
        QVERIFY2(failed.isEmpty(), qPrintable(actions.errorString())); QCOMPARE(done.size(), 2);
        QVERIFY(QFileInfo::exists(temp.filePath("Models/test 2.safetensors"))); QVERIFY(QFileInfo::exists(source));
        actions.rename("updated.safetensors"); QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 120000);
        QVERIFY2(failed.isEmpty(), qPrintable(actions.errorString())); QCOMPARE(done.size(), 3); QVERIFY(!QFileInfo::exists(source));
        actions.setPath(temp.filePath("Models/updated.safetensors")); actions.trash();
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 120000);
        QVERIFY2(failed.isEmpty(), qPrintable(actions.errorString())); QCOMPARE(done.size(), 4);
        QVERIFY(QFileInfo::exists(temp.filePath("Deleted/updated.safetensors")));
        actions.setPath(temp.filePath("Deleted/updated.safetensors")); actions.restore();
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 120000);
        QVERIFY2(failed.isEmpty(), qPrintable(actions.errorString())); QCOMPARE(done.size(), 5);
        QCOMPARE(done.last().first().toString(), temp.filePath("Models/updated.safetensors"));
        QVERIFY(QFileInfo::exists(temp.filePath("Models/updated.safetensors")));
        QVERIFY(!QFileInfo::exists(temp.filePath("Deleted/updated.safetensors")));
        QFile legacy(temp.filePath("Deleted/legacy.bin")); QVERIFY(legacy.open(QIODevice::WriteOnly)); legacy.write("legacy"); legacy.close();
        actions.setPath(legacy.fileName()); actions.restoreTo(QUrl::fromLocalFile(temp.filePath("Models") + "/"));
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 120000);
        QVERIFY2(failed.isEmpty(), qPrintable(actions.errorString())); QCOMPARE(done.size(), 6);
        QVERIFY(QFileInfo::exists(temp.filePath("Models/legacy.bin"))); QVERIFY(!QFileInfo::exists(legacy.fileName()));
    }
    void metadataMenuDoesNotPullAndCopyWaitsForSelectedObject() {
        QTemporaryDir temp(QDir::tempPath() + "/remote-actions-XXXXXX.societycontainer"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive); StorageMap map(*drive);
        QJsonObject entry{{"path", "models/remote.bin"}, {"kind", "file"}, {"size", "3"}, {"version", QString(64, 'a')}, {"resident", false}};
        QVERIFY(map.publish({entry})); FileActions actions; actions.setPath(temp.filePath("Models/remote.bin"));
        QTRY_VERIFY_WITH_TIMEOUT(actions.editable(), 15000); QVERIFY(map.pendingRequests().isEmpty());
        QSignalSpy done(&actions, &FileActions::completed); actions.copy();
        QTRY_COMPARE(map.pendingRequests().size(), 1); QCOMPARE(done.size(), 0); QVERIFY(actions.busy());
        const auto request = map.pendingRequests().first().toObject();
        QCOMPARE(request.value("objects").toArray().size(), 1);
        QCOMPARE(request.value("objects").toArray().first().toObject().value("path"), entry.value("path"));
        QFile file(actions.path()); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("new"); file.close();
        entry["resident"] = true; QVERIFY(map.publish({entry})); QVERIFY(map.finishRequest(request.value("id").toString()));
        QTRY_COMPARE(done.size(), 1); QVERIFY(!actions.busy());
        QCOMPARE(QGuiApplication::clipboard()->mimeData()->urls().first().toLocalFile(), actions.path());
    }
};
QTEST_MAIN(FileActionsTests)
#include "file_actions.moc"
