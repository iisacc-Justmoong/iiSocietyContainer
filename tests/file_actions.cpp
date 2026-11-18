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
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QLockFile>
#include "SignalRecorder.h"
#include "QtEventWait.h"
#include <QEvent>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include "../src/DeletedOrigins.h"
#include <filesystem>
#include <optional>
using namespace iiSocietyContainer;
class FileActionsTests : public QObject {
public:
    void pathCapabilitiesFollowTheCurrentSelection() {
        QTemporaryDir temp(QDir::tempPath() + "/file-access-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        CTEST_CHECK(SocietyDrive::create(temp.path()));
        const auto first = temp.filePath("Files/first.txt"), second = temp.filePath("Deleted/second.txt");
        for (const auto &path : {first, second}) {
            QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_CHECK(file.write("asset") > 0);
        }
        FileActions actions;
        actions.setPath(first);
        actions.setPath(second);
        CTEST_WAIT_VERIFY(actions.inDeleted() && actions.editable(), 15000);
        actions.setPath(QString());
        CTEST_CHECK(!actions.inDeleted()); CTEST_CHECK(!actions.editable());
    }
    void unrelatedDeletesAreAcceptedAndCompleteWhileAnotherItemIsBusy() {
        QTemporaryDir temp(QDir::tempPath() + "/parallel-delete-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        const auto first = temp.filePath("Deleted/first.bin"), second = temp.filePath("Deleted/second.bin");
        for (const auto &path : {first, second}) {
            QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_CHECK(file.write("asset") > 0);
        }
        detail::DeletedOrigins held(*drive); QString error;
        CTEST_CHECK_MESSAGE(held.lockPaths({first}, &error, 0), qPrintable(error));
        FileActions actions;
        society_ctest::SignalRecorder finished(&actions, &FileActions::operationFinished);
        actions.setPath(first); actions.remove();
        actions.setPath(second); actions.remove();
        CTEST_COMPARE(actions.activeCount(), 2);
        CTEST_CHECK(actions.busyForPath(first)); CTEST_CHECK(actions.busyForPath(second));
        QElapsedTimer elapsed; elapsed.start();
        while (QFileInfo::exists(second) && elapsed.elapsed() < 1500) society_ctest::processFor(10);
        const bool secondFinishedWhileFirstBlocked = !QFileInfo::exists(second) && QFileInfo::exists(first);
        held.unlock();
        CTEST_CHECK_MESSAGE(secondFinishedWhileFirstBlocked, "An unrelated removal waited for the first asset's lock");
        CTEST_WAIT_VERIFY(!actions.busy(), 15000);
        CTEST_COMPARE(finished.size(), 2);
        CTEST_CHECK(!QFileInfo::exists(first)); CTEST_CHECK(!QFileInfo::exists(second));
        QSet<QString> sources;
        for (const auto &event : finished) { sources.insert(event.at(0).toString()); CTEST_CHECK(event.at(2).toString().isEmpty()); }
        CTEST_COMPARE(sources, QSet<QString>({first, second}));
    }
    void closingAnAdapterDoesNotCancelAnAcceptedRemoval() {
        QTemporaryDir temp(QDir::tempPath() + "/detached-delete-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        const auto path = temp.filePath("Deleted/pending.txt");
        QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_CHECK(file.write("asset") > 0); file.close();
        detail::DeletedOrigins held(*drive); QString error;
        CTEST_CHECK_MESSAGE(held.lockPaths({path}, &error, 0), qPrintable(error));
        auto actions = std::make_unique<FileActions>();
        actions->removePath(path);
        CTEST_COMPARE(actions->activeCount(), 1);
        QElapsedTimer elapsed; elapsed.start(); actions.reset();
        CTEST_CHECK_MESSAGE(elapsed.elapsed() < 500, "Closing the view blocked on the removal worker");
        held.unlock();
        CTEST_WAIT_VERIFY(!QFileInfo::exists(path), 15000);
    }
    void renamesLocalModelWithoutDownloadingOrReadingPayload() {
        QTemporaryDir temp(QDir::tempPath() + "/rename-model-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        CTEST_CHECK(QDir().mkpath(temp.filePath("Models/Checkpoint")));
        const auto source = temp.filePath("Models/Checkpoint/old.safetensors");
        const auto destination = temp.filePath("Models/Checkpoint/new.safetensors");
        QFile file(source); CTEST_CHECK(file.open(QIODevice::WriteOnly));
        const auto size = 8LL * 1024 * 1024 * 1024;
        CTEST_CHECK(file.resize(size)); file.close();
        StorageMap map(*drive);
        CTEST_CHECK(map.publish({QJsonObject{{"path", "models/Checkpoint/old.safetensors"}, {"kind", "file"},
            {"size", "1"}, {"resident", false}, {"version", QString(64, 'a')}}}));
        FileActions actions;
        society_ctest::SignalRecorder completed(&actions, &FileActions::completed); society_ctest::SignalRecorder failed(&actions, &FileActions::failed);
        actions.setPath(source); actions.rename("new.safetensors");
        CTEST_WAIT_VERIFY(!actions.busy(), 30000);
        CTEST_CHECK_MESSAGE(failed.isEmpty(), qPrintable(actions.errorString()));
        CTEST_COMPARE(completed.size(), 1);
        CTEST_COMPARE(completed.first().first().toString(), destination);
        CTEST_CHECK(!QFileInfo::exists(source));
        CTEST_COMPARE(QFileInfo(destination).size(), size);
        CTEST_CHECK(map.pendingRequests().isEmpty());
        const auto storage = SharedStorage::open(temp.path()); CTEST_CHECK(storage);
        const auto tracking = storage->fileTracking(StoreSection::Models, "Checkpoint/new.safetensors");
        // The model catalog can already describe the working copy without a
        // managed content revision or reading the weights into RAW storage.
        CTEST_COMPARE(tracking.value("status").toString(), QStringLiteral("ready"));
        CTEST_COMPARE(tracking.value("resourceStatus").toString(), QStringLiteral("working-copy-only"));
        CTEST_COMPARE(tracking.value("resource").toObject().value("contentRevisionStatus").toString(), QStringLiteral("uncommitted"));
        CTEST_CHECK(!tracking.contains("rawKey"));
        const auto store = ModelStore::open(temp.path()); CTEST_CHECK(store);
        QString error;
        CTEST_COMPARE(store->resolve("Checkpoint/old.safetensors", &error), destination);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
    }

    void renameKeepsTrackedIdentityAndDefersChangedContent() {
        QTemporaryDir temp(QDir::tempPath() + "/rename-tracked-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        CTEST_CHECK(SocietyDrive::create(temp.path()));
        CTEST_CHECK(QDir().mkpath(temp.filePath("Models/Checkpoint")));
        const auto source = temp.filePath("Models/Checkpoint/old.safetensors");
        const auto destination = temp.filePath("Models/Checkpoint/new.safetensors");
        QFile file(source); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_CHECK(file.write("original weights") > 0); file.close();
        const auto storage = SharedStorage::open(temp.path()); CTEST_CHECK(storage);
        QString error;
        CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::Models, "Checkpoint/old.safetensors", &error), qPrintable(error));
        const auto before = storage->fileTracking(StoreSection::Models, "Checkpoint/old.safetensors", &error);
        CTEST_CHECK_MESSAGE(!before.isEmpty(), qPrintable(error));
        const auto key = before.value("objectKey");
        const auto revision = before.value("resource").toObject().value("contentRevision");
        CTEST_CHECK(!key.toString().isEmpty()); CTEST_CHECK(!revision.toString().isEmpty());
        CTEST_CHECK(file.open(QIODevice::Append)); CTEST_CHECK(file.write(" changed") > 0); file.close();
        FileActions actions; society_ctest::SignalRecorder completed(&actions, &FileActions::completed); society_ctest::SignalRecorder failed(&actions, &FileActions::failed);
        actions.setPath(source); actions.rename("new.safetensors");
        CTEST_WAIT_VERIFY(!actions.busy(), 30000);
        CTEST_CHECK_MESSAGE(failed.isEmpty(), qPrintable(actions.errorString())); CTEST_COMPARE(completed.size(), 1);
        const auto moved = storage->fileTracking(StoreSection::Models, "Checkpoint/new.safetensors", &error);
        CTEST_CHECK_MESSAGE(!moved.isEmpty(), qPrintable(error));
        CTEST_COMPARE(moved.value("objectKey"), key);
        CTEST_COMPARE(moved.value("resource").toObject().value("contentRevision"), revision);
        CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::Models, "Checkpoint/new.safetensors", &error), qPrintable(error));
        const auto revised = storage->fileTracking(StoreSection::Models, "Checkpoint/new.safetensors", &error);
        CTEST_COMPARE(revised.value("objectKey"), key);
        CTEST_CHECK(revised.value("resource").toObject().value("contentRevision") != revision);
        CTEST_CHECK(!QFileInfo::exists(source)); CTEST_CHECK(QFileInfo::exists(destination));
    }

    void renameChangesOnlyLetterCaseOnCaseInsensitiveStorage() {
        QTemporaryDir temp(QDir::tempPath() + "/rename-case-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        CTEST_CHECK(SocietyDrive::create(temp.path()));
        CTEST_CHECK(QDir().mkpath(temp.filePath("Models/Checkpoint")));
        const auto source = temp.filePath("Models/Checkpoint/CaseModel.safetensors");
        const auto destination = temp.filePath("Models/Checkpoint/casemodel.safetensors");
        QFile file(source); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_CHECK(file.write("weights") > 0); file.close();
        if (!QFileInfo::exists(destination)) society_ctest::skip("This filesystem distinguishes filename case.");
        std::error_code ec;
        CTEST_CHECK(std::filesystem::equivalent(std::filesystem::path(source.toStdU16String()),
                                            std::filesystem::path(destination.toStdU16String()), ec));
        CTEST_CHECK(!ec);
        const auto storage = SharedStorage::open(temp.path()); CTEST_CHECK(storage);
        QString error;
        CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::Models, "Checkpoint/CaseModel.safetensors", &error), qPrintable(error));
        const auto before = storage->fileTracking(StoreSection::Models, "Checkpoint/CaseModel.safetensors").value("objectKey");
        CTEST_CHECK(!before.toString().isEmpty());
        const auto classifications = temp.filePath(".society-runtime/classifications/models/Checkpoint");
        QFile originalClassification(classifications + "/CaseModel.safetensors.iisacc.json");
        CTEST_CHECK(originalClassification.open(QIODevice::ReadOnly));
        const auto originalBytes = originalClassification.readAll(); originalClassification.close();
        CTEST_CHECK(!originalBytes.isEmpty());
        FileActions actions; society_ctest::SignalRecorder completed(&actions, &FileActions::completed); society_ctest::SignalRecorder failed(&actions, &FileActions::failed);
        actions.setPath(source); actions.rename("casemodel.safetensors");
        CTEST_WAIT_VERIFY(!actions.busy(), 30000);
        CTEST_CHECK_MESSAGE(failed.isEmpty(), qPrintable(actions.errorString()));
        CTEST_COMPARE(completed.size(), 1); CTEST_COMPARE(completed.first().first().toString(), destination);
        const auto names = QDir(QFileInfo(source).absolutePath()).entryList(QDir::Files | QDir::NoDotAndDotDot);
        CTEST_CHECK(names.contains("casemodel.safetensors", Qt::CaseSensitive));
        CTEST_CHECK(!names.contains("CaseModel.safetensors", Qt::CaseSensitive));
        CTEST_CHECK(!names.contains("casemodel.safetensors.iisacc.json", Qt::CaseSensitive));
        const auto classificationNames = QDir(classifications).entryList(QDir::Files | QDir::NoDotAndDotDot);
        CTEST_CHECK(classificationNames.contains("casemodel.safetensors.iisacc.json", Qt::CaseSensitive));
        CTEST_CHECK(!classificationNames.contains("CaseModel.safetensors.iisacc.json", Qt::CaseSensitive));
        QFile renamedClassification(classifications + "/casemodel.safetensors.iisacc.json");
        CTEST_CHECK(renamedClassification.open(QIODevice::ReadOnly));
        QJsonParseError originalError, renamedError;
        const auto originalDocument = QJsonDocument::fromJson(originalBytes, &originalError);
        const auto renamedDocument = QJsonDocument::fromJson(renamedClassification.readAll(), &renamedError);
        CTEST_CHECK(originalError.error == QJsonParseError::NoError && originalDocument.isObject());
        CTEST_CHECK(renamedError.error == QJsonParseError::NoError && renamedDocument.isObject());
        const auto originalObject = originalDocument.object(), renamedObject = renamedDocument.object();
        for (const auto *key : {"format", "schema", "fileKey", "metadata", "collections"})
            CTEST_COMPARE(renamedObject.value(key), originalObject.value(key));
        CTEST_COMPARE(originalObject.value("path").toString(), QStringLiteral("Checkpoint/CaseModel.safetensors"));
        CTEST_COMPARE(renamedObject.value("path").toString(), QStringLiteral("Checkpoint/casemodel.safetensors"));
        CTEST_CHECK(renamedObject.value("revision").toInteger() >= originalObject.value("revision").toInteger());
        for (const auto &name : names) CTEST_CHECK(!name.startsWith("SocietyRename-"));
        CTEST_COMPARE(storage->fileTracking(StoreSection::Models, "Checkpoint/casemodel.safetensors").value("objectKey"), before);
        const auto store = ModelStore::open(temp.path()); CTEST_CHECK(store);
        CTEST_COMPARE(store->resolve("Checkpoint/CaseModel.safetensors", &error), destination);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
    }

    void renameMovesModelPackagesAndKeepsTrackedChildren() {
        QTemporaryDir temp(QDir::tempPath() + "/rename-package-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        CTEST_CHECK(SocietyDrive::create(temp.path()));
        const auto source = temp.filePath("Models/LLM/old-package");
        const auto destination = temp.filePath("Models/LLM/new-package");
        CTEST_CHECK(QDir().mkpath(source));
        QFile config(source + "/config.json"); CTEST_CHECK(config.open(QIODevice::WriteOnly));
        CTEST_CHECK(config.write("{}") > 0); config.close();
        QFile weights(source + "/model.bin"); CTEST_CHECK(weights.open(QIODevice::WriteOnly));
        CTEST_CHECK(weights.write("model weights") > 0); weights.close();
        const auto storage = SharedStorage::open(temp.path()); CTEST_CHECK(storage);
        QString error;
        CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::Models, "LLM/old-package/model.bin", &error), qPrintable(error));
        const auto before = storage->fileTracking(StoreSection::Models, "LLM/old-package/model.bin").value("objectKey");
        CTEST_CHECK(!before.toString().isEmpty());
        const auto classifications = temp.filePath(".society-runtime/classifications/models/LLM");
        QFile originalClassification(classifications + "/old-package/model.bin.iisacc.json");
        CTEST_CHECK(originalClassification.open(QIODevice::ReadOnly));
        const auto originalBytes = originalClassification.readAll(); originalClassification.close();
        CTEST_CHECK(!originalBytes.isEmpty());
        FileActions actions; society_ctest::SignalRecorder completed(&actions, &FileActions::completed); society_ctest::SignalRecorder failed(&actions, &FileActions::failed);
        actions.setPath(source); actions.rename("new-package");
        CTEST_WAIT_VERIFY(!actions.busy(), 30000);
        CTEST_CHECK_MESSAGE(failed.isEmpty(), qPrintable(actions.errorString()));
        CTEST_COMPARE(completed.size(), 1); CTEST_COMPARE(completed.first().first().toString(), destination);
        CTEST_CHECK(!QFileInfo::exists(source)); CTEST_CHECK(QFileInfo::exists(destination + "/config.json"));
        CTEST_CHECK(!QFileInfo::exists(destination + "/model.bin.iisacc.json"));
        CTEST_CHECK(!QFileInfo::exists(classifications + "/old-package/model.bin.iisacc.json"));
        QFile renamedClassification(classifications + "/new-package/model.bin.iisacc.json");
        CTEST_CHECK(renamedClassification.open(QIODevice::ReadOnly));
        CTEST_COMPARE(renamedClassification.readAll(), originalBytes);
        CTEST_COMPARE(storage->fileTracking(StoreSection::Models, "LLM/new-package/model.bin").value("objectKey"), before);
        const auto store = ModelStore::open(temp.path()); CTEST_CHECK(store);
        CTEST_COMPARE(store->resolve("LLM/old-package", &error), destination);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
    }

    void renameWaitsForTheIndexWithoutMovingTheFileEarly() {
        QTemporaryDir temp(QDir::tempPath() + "/rename-lock-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive);
        CTEST_CHECK(QDir().mkpath(temp.filePath("Models/Checkpoint")));
        const auto source = temp.filePath("Models/Checkpoint/old.safetensors");
        const auto destination = temp.filePath("Models/Checkpoint/new.safetensors");
        QFile file(source); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_CHECK(file.write("weights") > 0); file.close();
        const auto storage = SharedStorage::open(temp.path()); CTEST_CHECK(storage);
        QString error;
        CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::Models, "Checkpoint/old.safetensors", &error), qPrintable(error));
        const auto original = storage->fileTracking(StoreSection::Models, "Checkpoint/old.safetensors").value("objectKey");
        CTEST_CHECK(!original.toString().isEmpty());
        FileActions actions; society_ctest::SignalRecorder completed(&actions, &FileActions::completed); society_ctest::SignalRecorder failed(&actions, &FileActions::failed);
        const auto id = drive->storageKind() == SocietyDrive::StorageKind::LogicalDisk ? drive->storageIdentifier() : drive->identifier();
        const auto path = FileRepository::packagePath(std::filesystem::path(temp.path().toStdU16String()), id.toStdString()) / "operations";
        std::optional<iiFileProvider::TemporaryLease> lock;
        lock.emplace(path, iiFileProvider::TemporaryLease::Mode::TryExclusive); CTEST_CHECK(lock->locked());
        actions.setPath(source); actions.rename("new.safetensors");
        society_ctest::processFor(250);
        const bool waited = actions.busy() && QFileInfo::exists(source) && !QFileInfo::exists(destination);
        lock.reset();
        CTEST_CHECK(waited);
        CTEST_WAIT_VERIFY(!actions.busy(), 30000);
        CTEST_CHECK_MESSAGE(failed.isEmpty(), qPrintable(actions.errorString())); CTEST_COMPARE(completed.size(), 1);
        CTEST_COMPARE(storage->fileTracking(StoreSection::Models, "Checkpoint/new.safetensors").value("objectKey"), original);
    }

    void renameRejectsRemoteOnlyAndReplicaFilesWithoutDownloading() {
        QTemporaryDir temp(QDir::tempPath() + "/rename-replica-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto created = SocietyDrive::create(temp.path()); CTEST_CHECK(created);
        CTEST_CHECK(SocietyDrive::adoptReplicaIdentity(temp.path(), created->identifier(), created->identifier()));
        CTEST_CHECK(SocietyDrive::completeReplica(temp.path(), created->identifier()));
        const auto drive = SocietyDrive::open(temp.path()); CTEST_CHECK(drive);
        CTEST_CHECK(QDir().mkpath(temp.filePath("Models/Checkpoint")));
        const auto path = temp.filePath("Models/Checkpoint/model.safetensors");
        StorageMap map(*drive); CTEST_CHECK(!map.isLocalAuthority());
        CTEST_CHECK(map.publish({QJsonObject{{"path", "models/Checkpoint/model.safetensors"}, {"kind", "file"},
            {"size", "7"}, {"resident", false}, {"version", QString(64, 'a')}}}));
        FileActions actions; society_ctest::SignalRecorder failed(&actions, &FileActions::failed);
        actions.setPath(path); actions.rename("new.safetensors");
        CTEST_WAIT_VERIFY(!actions.busy(), 15000);
        CTEST_COMPARE(failed.size(), 1); CTEST_CHECK(map.pendingRequests().isEmpty());
        QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write("weights"), 7); file.close();
        actions.rename("new.safetensors");
        CTEST_WAIT_VERIFY(!actions.busy(), 15000);
        CTEST_COMPARE(failed.size(), 2); CTEST_CHECK(actions.errorString().contains("host"));
        CTEST_CHECK(QFileInfo::exists(path)); CTEST_CHECK(!QFileInfo::exists(temp.filePath("Models/Checkpoint/new.safetensors")));
        CTEST_CHECK(map.pendingRequests().isEmpty());
    }

    void deletionNeverQueuesDownloads() {
        QTemporaryDir temp(QDir::tempPath() + "/delete-no-pull-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive); StorageMap map(*drive);
        const auto source = temp.filePath("Models/Checkpoint/model.safetensors");
        CTEST_CHECK(QDir().mkpath(temp.filePath("Models/Checkpoint")));
        QFile file(source); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_CHECK(file.resize(8LL * 1024 * 1024 * 1024)); file.close();
        QJsonObject entry{{"path", "models/Checkpoint/model.safetensors"}, {"kind", "file"},
            {"size", "1"}, {"resident", false}, {"version", QString(64, 'a')}};
        CTEST_CHECK(map.publish({entry}));
        QLockFile sync(temp.filePath(".society-sync/operation.lock")); CTEST_CHECK(sync.tryLock());
        FileActions actions; society_ctest::SignalRecorder done(&actions, &FileActions::completed); society_ctest::SignalRecorder failed(&actions, &FileActions::failed);
        actions.setPath(source); actions.trash();
        CTEST_WAIT_VERIFY(!actions.busy(), 1500);
        CTEST_COMPARE(done.size(), 1); CTEST_CHECK(failed.isEmpty()); CTEST_CHECK(map.pendingRequests().isEmpty());
        const auto deleted = done.first().first().toString(); CTEST_CHECK(QFileInfo::exists(deleted));
        entry["path"] = "deleted/model.safetensors"; CTEST_CHECK(map.publish({entry}));
        actions.setPath(deleted); actions.remove();
        CTEST_WAIT_VERIFY(!actions.busy(), 1500);
        CTEST_COMPARE(done.size(), 2); CTEST_CHECK(failed.isEmpty()); CTEST_CHECK(map.pendingRequests().isEmpty());
        CTEST_CHECK(!QFileInfo::exists(deleted));
        // Remote-only files cannot be removed locally, and must not be fetched to delete them.
        entry["path"] = "models/remote.safetensors"; CTEST_CHECK(map.publish({entry}));
        actions.setPath(temp.filePath("Models/remote.safetensors")); actions.trash();
        CTEST_WAIT_VERIFY(!actions.busy(), 1500);
        CTEST_COMPARE(failed.size(), 1); CTEST_COMPARE(done.size(), 2); CTEST_CHECK(map.pendingRequests().isEmpty());
    }
    void copiesUrlsAndPastesWithoutOverwriting() {
        QTemporaryDir temp(QDir::tempPath() + "/clipboard-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        CTEST_CHECK(SocietyDrive::create(temp.path()));
        const auto source = temp.filePath("Models/test.safetensors");
        QFile file(source); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("weights"); file.close();
        FileActions actions; society_ctest::SignalRecorder done(&actions, &FileActions::completed); society_ctest::SignalRecorder failed(&actions, &FileActions::failed);
        actions.setPath(source); actions.copy();
        CTEST_WAIT_VERIFY(!actions.busy(), 120000); CTEST_CHECK_MESSAGE(failed.isEmpty(), qPrintable(actions.errorString())); CTEST_COMPARE(done.size(), 1);
        CTEST_COMPARE(QGuiApplication::clipboard()->mimeData()->urls(), QList<QUrl>{QUrl::fromLocalFile(source)});
        CTEST_CHECK(actions.canPaste());
        actions.paste(temp.filePath("Models")); CTEST_WAIT_VERIFY(!actions.busy(), 120000);
        CTEST_CHECK_MESSAGE(failed.isEmpty(), qPrintable(actions.errorString())); CTEST_COMPARE(done.size(), 2);
        CTEST_CHECK(QFileInfo::exists(temp.filePath("Models/test 2.safetensors"))); CTEST_CHECK(QFileInfo::exists(source));
        actions.rename("updated.safetensors"); CTEST_WAIT_VERIFY(!actions.busy(), 120000);
        CTEST_CHECK_MESSAGE(failed.isEmpty(), qPrintable(actions.errorString())); CTEST_COMPARE(done.size(), 3); CTEST_CHECK(!QFileInfo::exists(source));
        actions.setPath(temp.filePath("Models/updated.safetensors")); actions.trash();
        CTEST_WAIT_VERIFY(!actions.busy(), 120000);
        CTEST_CHECK_MESSAGE(failed.isEmpty(), qPrintable(actions.errorString())); CTEST_COMPARE(done.size(), 4);
        CTEST_CHECK(QFileInfo::exists(temp.filePath("Deleted/updated.safetensors")));
        actions.setPath(temp.filePath("Deleted/updated.safetensors")); actions.restore();
        CTEST_WAIT_VERIFY(!actions.busy(), 120000);
        CTEST_CHECK_MESSAGE(failed.isEmpty(), qPrintable(actions.errorString())); CTEST_COMPARE(done.size(), 5);
        CTEST_COMPARE(done[done.size()-1].first().toString(), temp.filePath("Models/updated.safetensors"));
        CTEST_CHECK(QFileInfo::exists(temp.filePath("Models/updated.safetensors")));
        CTEST_CHECK(!QFileInfo::exists(temp.filePath("Deleted/updated.safetensors")));
        QFile legacy(temp.filePath("Deleted/legacy.bin")); CTEST_CHECK(legacy.open(QIODevice::WriteOnly)); legacy.write("legacy"); legacy.close();
        actions.setPath(legacy.fileName()); actions.restoreTo(QUrl::fromLocalFile(temp.filePath("Models") + "/"));
        CTEST_WAIT_VERIFY(!actions.busy(), 120000);
        CTEST_CHECK_MESSAGE(failed.isEmpty(), qPrintable(actions.errorString())); CTEST_COMPARE(done.size(), 6);
        CTEST_CHECK(QFileInfo::exists(temp.filePath("Models/legacy.bin"))); CTEST_CHECK(!QFileInfo::exists(legacy.fileName()));
    }
    void metadataMenuDoesNotPullAndCopyWaitsForSelectedObject() {
        QTemporaryDir temp(QDir::tempPath() + "/remote-actions-XXXXXX.societycontainer"); CTEST_CHECK(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); CTEST_CHECK(drive); StorageMap map(*drive);
        QJsonObject entry{{"path", "models/remote.bin"}, {"kind", "file"}, {"size", "3"}, {"version", QString(64, 'a')}, {"resident", false}};
        CTEST_CHECK(map.publish({entry})); FileActions actions; actions.setPath(temp.filePath("Models/remote.bin"));
        CTEST_WAIT_VERIFY(actions.editable(), 15000); CTEST_CHECK(map.pendingRequests().isEmpty());
        society_ctest::SignalRecorder done(&actions, &FileActions::completed); actions.copy();
        CTEST_WAIT_COMPARE(map.pendingRequests().size(), 1); CTEST_COMPARE(done.size(), 0); CTEST_CHECK(actions.busy());
        const auto request = map.pendingRequests().first().toObject();
        CTEST_COMPARE(request.value("objects").toArray().size(), 1);
        CTEST_COMPARE(request.value("objects").toArray().first().toObject().value("path"), entry.value("path"));
        QFile file(actions.path()); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("new"); file.close();
        entry["resident"] = true; CTEST_CHECK(map.publish({entry})); CTEST_CHECK(map.finishRequest(request.value("id").toString()));
        CTEST_WAIT_COMPARE(done.size(), 1); CTEST_CHECK(!actions.busy());
        CTEST_COMPARE(QGuiApplication::clipboard()->mimeData()->urls().first().toLocalFile(), actions.path());
    }
};
int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    society_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    FileActionsTests tests;
    runner.run("pathCapabilitiesFollowTheCurrentSelection", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.pathCapabilitiesFollowTheCurrentSelection(); });
    runner.run("unrelatedDeletesAreAcceptedAndCompleteWhileAnotherItemIsBusy", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.unrelatedDeletesAreAcceptedAndCompleteWhileAnotherItemIsBusy(); });
    runner.run("closingAnAdapterDoesNotCancelAnAcceptedRemoval", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.closingAnAdapterDoesNotCancelAnAcceptedRemoval(); });
    runner.run("renamesLocalModelWithoutDownloadingOrReadingPayload", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.renamesLocalModelWithoutDownloadingOrReadingPayload(); });
    runner.run("renameKeepsTrackedIdentityAndDefersChangedContent", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.renameKeepsTrackedIdentityAndDefersChangedContent(); });
    runner.run("renameChangesOnlyLetterCaseOnCaseInsensitiveStorage", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.renameChangesOnlyLetterCaseOnCaseInsensitiveStorage(); });
    runner.run("renameMovesModelPackagesAndKeepsTrackedChildren", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.renameMovesModelPackagesAndKeepsTrackedChildren(); });
    runner.run("renameWaitsForTheIndexWithoutMovingTheFileEarly", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.renameWaitsForTheIndexWithoutMovingTheFileEarly(); });
    runner.run("renameRejectsRemoteOnlyAndReplicaFilesWithoutDownloading", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.renameRejectsRemoteOnlyAndReplicaFilesWithoutDownloading(); });
    runner.run("deletionNeverQueuesDownloads", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.deletionNeverQueuesDownloads(); });
    runner.run("copiesUrlsAndPastesWithoutOverwriting", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.copiesUrlsAndPastesWithoutOverwriting(); });
    runner.run("metadataMenuDoesNotPullAndCopyWaitsForSelectedObject", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.metadataMenuDoesNotPullAndCopyWaitsForSelectedObject(); });
    return runner.result();
}
