#include "CTestSupport.h"
#include "QtEventWait.h"
#include "SignalRecorder.h"
#include <QCoreApplication>
#include "FileJobService.h"
#include "ContainerJobStorage.h"
#include "SocietyDrive.h"
#include "FileOperations.h"
#include <FileCatalog.h>
#include <TemporaryLease.h>
#include <Job.h>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>

using namespace iiSocietyContainer;
class NativeFileJobsTests final : public QObject {
public:
    void independentCopiesWaitForTheContainerCommitLock() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-lock-XXXXXX"); CTEST_CHECK(fixture.isValid());
        const auto root=fixture.filePath("drive.societycontainer"); CTEST_CHECK(QDir().mkdir(root)); CTEST_CHECK(SocietyDrive::create(root));
        const auto outside=fixture.filePath("outside"); CTEST_CHECK(QDir().mkdir(outside));
        for (const auto &name : {"first.txt","second.txt"}) {
            QFile source(outside+"/"+name); CTEST_CHECK(source.open(QIODevice::WriteOnly)); CTEST_COMPARE(source.write("content"),qint64(7));
        }
        CTEST_CHECK(QDir().mkpath(root+"/.society-sync")); QLockFile lock(root+"/.society-sync/operation.lock");
        lock.setStaleLockTime(0); CTEST_CHECK(lock.tryLock());
        FileJobService jobs(fixture.filePath("jobs")); society_ctest::SignalRecorder recorded(&jobs,&FileJobService::recorded);
        society_ctest::SignalRecorder done(&jobs,&FileJobService::finished); QString error;
        const auto first=jobs.submit(FileJobService::Kind::Copy,root,outside+"/first.txt","Files",&error); CTEST_CHECK_MESSAGE(!first.isEmpty(),qPrintable(error));
        const auto second=jobs.submit(FileJobService::Kind::Copy,root,outside+"/second.txt","Files",&error); CTEST_CHECK_MESSAGE(!second.isEmpty(),qPrintable(error));
        CTEST_WAIT_COMPARE_TIMEOUT(recorded.size(),2,30000);
        society_ctest::processFor(3500); const auto earlyError=done.isEmpty()?QString():done.first()[2].toString();
        CTEST_CHECK_MESSAGE(done.isEmpty(),qPrintable(earlyError));
        lock.unlock(); CTEST_WAIT_COMPARE_TIMEOUT(done.size(),2,120000);
        for (const auto &event : done) CTEST_CHECK_MESSAGE(event[2].toString().isEmpty(),qPrintable(event[2].toString()));
        for (const auto &name : {"first.txt","second.txt"}) {
            QFile copy(root+"/Files/"+name); CTEST_CHECK(copy.open(QIODevice::ReadOnly)); CTEST_COMPARE(copy.readAll(),QByteArray("content"));
        }
    }
    void serviceStopInterruptsAWaitingCopyWithoutPublishingIt() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-stop-XXXXXX"); CTEST_CHECK(fixture.isValid());
        const auto root=fixture.filePath("drive.societycontainer"); CTEST_CHECK(QDir().mkdir(root)); CTEST_CHECK(SocietyDrive::create(root));
        QFile source(fixture.filePath("external.txt")); CTEST_CHECK(source.open(QIODevice::WriteOnly)); source.write("data"); source.close();
        CTEST_CHECK(QDir().mkpath(root+"/.society-sync")); QLockFile lock(root+"/.society-sync/operation.lock");
        lock.setStaleLockTime(0); CTEST_CHECK(lock.tryLock());
        auto jobs=std::make_unique<FileJobService>(fixture.filePath("jobs")); society_ctest::SignalRecorder recorded(jobs.get(),&FileJobService::recorded);
        QString error; const auto id=jobs->submit(FileJobService::Kind::Copy,root,source.fileName(),"Files",&error);
        CTEST_CHECK_MESSAGE(!id.isEmpty(),qPrintable(error)); CTEST_WAIT_COMPARE_TIMEOUT(recorded.size(),1,30000);
        QElapsedTimer elapsed; elapsed.start(); jobs.reset(); CTEST_CHECK(elapsed.elapsed()<1000);
        ContainerJobStorage storage(std::filesystem::path(fixture.filePath("jobs").toStdU16String()));
        const auto interrupted=[&] {
            try { auto store=storage.open(false); const auto item=store->item(id.toStdString(),"item");
                return item && item->state==ii::job::ItemState::failed && item->retryable; }
            catch (...) { return false; }
        };
        CTEST_WAIT_VERIFY(interrupted(),30000);
        CTEST_CHECK(!QFileInfo::exists(root+"/Files/external.txt")); lock.unlock();
    }
    void invalidCommitLockFailsWithoutWaitingIndefinitely() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-bad-lock-XXXXXX"); CTEST_CHECK(fixture.isValid());
        const auto root=fixture.filePath("drive.societycontainer"); CTEST_CHECK(QDir().mkdir(root)); CTEST_CHECK(SocietyDrive::create(root));
        QFile source(fixture.filePath("external.txt")); CTEST_CHECK(source.open(QIODevice::WriteOnly)); source.write("data"); source.close();
        CTEST_CHECK(QDir().mkpath(root+"/.society-sync/operation.lock"));
        FileJobService jobs(fixture.filePath("jobs")); society_ctest::SignalRecorder done(&jobs,&FileJobService::finished); QString error;
        const auto id=jobs.submit(FileJobService::Kind::Copy,root,source.fileName(),"Files",&error);
        CTEST_CHECK_MESSAGE(!id.isEmpty(),qPrintable(error)); CTEST_WAIT_COMPARE_TIMEOUT(done.size(),1,30000);
        CTEST_CHECK_MESSAGE(done[0][2].toString().contains("lock is unavailable"),qPrintable(done[0][2].toString()));
        CTEST_CHECK(!QFileInfo::exists(root+"/Files/external.txt")); CTEST_CHECK(QFileInfo::exists(source.fileName()));
    }
    void copiesExternalAssetAndCompanionsAsDurableJob() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-XXXXXX"); CTEST_CHECK(fixture.isValid());
        const auto root=fixture.filePath("drive.societycontainer"); CTEST_CHECK(QDir().mkdir(root)); CTEST_CHECK(SocietyDrive::create(root));
        const auto outside=fixture.filePath("outside"); CTEST_CHECK(QDir().mkdir(outside));
        QFile source(outside+"/asset.txt"); CTEST_CHECK(source.open(QIODevice::WriteOnly)); source.write("original"); source.close();
        iiFileProvider::FileCatalog catalog(std::filesystem::path(outside.toStdU16String()));
        iiFileProvider::FileMetadata sourceMetadata; sourceMetadata.title="Imported asset";
        CTEST_CHECK(catalog.registerFile("asset.txt",sourceMetadata).metadataWritten);
        QFile metadata(source.fileName()+".iisacc.json"); CTEST_CHECK(QFileInfo::exists(metadata.fileName()));
        FileJobService jobs(fixture.filePath("jobs")); society_ctest::SignalRecorder done(&jobs,&FileJobService::finished); QString error;
        const auto canonical=QFileInfo(source).canonicalFilePath();
        const auto id=jobs.submit(FileJobService::Kind::Copy,root,canonical,"Files",&error);
        CTEST_CHECK_MESSAGE(!id.isEmpty(),qPrintable(error)); CTEST_WAIT_COMPARE_TIMEOUT(done.size(),1,120000);
        CTEST_CHECK_MESSAGE(done[0][2].toString().isEmpty(),qPrintable(done[0][2].toString()));
        CTEST_COMPARE(done[0][1].toString(),QStringLiteral("Files/asset.txt"));
        CTEST_CHECK(QFileInfo::exists(source.fileName())); CTEST_CHECK(QFileInfo::exists(metadata.fileName()));
        CTEST_CHECK(QFileInfo::exists(root+"/Files/asset.txt")); CTEST_CHECK(!QFileInfo::exists(root+"/Files/asset.txt.iisacc.json"));
        QFile importedMetadata(root+"/.society-runtime/classifications/files/asset.txt.iisacc.json");
        CTEST_CHECK(importedMetadata.open(QIODevice::ReadOnly));
        CTEST_COMPARE(QJsonDocument::fromJson(importedMetadata.readAll()).object().value("metadata").toObject().value("title").toString(),QStringLiteral("Imported asset"));
        society_ctest::SignalRecorder inspected(&jobs,&FileJobService::inspected); jobs.inspect("copy",id);
        CTEST_WAIT_COMPARE_TIMEOUT(inspected.size(),1,10000);
        CTEST_COMPARE(inspected[0][1].toMap().value("state").toString(),QStringLiteral("succeeded"));
        CTEST_CHECK(jobs.submit(FileJobService::Kind::Copy,root,canonical,"Files/../Models",&error).isEmpty());
        CTEST_CHECK(jobs.submit(FileJobService::Kind::Copy,root,"relative.txt","Files",&error).isEmpty());
        const auto protectedId=jobs.submit(FileJobService::Kind::Copy,root,QFileInfo(metadata).canonicalFilePath(),"Files",&error);
        CTEST_CHECK_MESSAGE(!protectedId.isEmpty(),qPrintable(error)); CTEST_WAIT_COMPARE_TIMEOUT(done.size(),2,30000);
        CTEST_CHECK(!done[1][2].toString().isEmpty());
    }
    void renamePersistsAndMovesCompanions() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-XXXXXX"); CTEST_CHECK(fixture.isValid());
        const auto root=fixture.filePath("drive.societycontainer"); CTEST_CHECK(QDir().mkdir(root));
        const auto drive=SocietyDrive::create(root); CTEST_CHECK(drive);
        const auto models=root+"/Models/Other"; CTEST_CHECK(QDir().mkpath(models));
        QFile file(models+"/model.safetensors"); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("fixture"); file.close();
        iiFileProvider::FileCatalog catalog(std::filesystem::path((root+"/Models").toStdU16String()));
        iiFileProvider::FileMetadata modelMetadata; modelMetadata.title="Original model";
        CTEST_CHECK(catalog.registerFile("Other/model.safetensors",modelMetadata).metadataWritten);
        FileJobService jobs(fixture.filePath("jobs")); society_ctest::SignalRecorder recorded(&jobs,&FileJobService::recorded);
        society_ctest::SignalRecorder finished(&jobs,&FileJobService::finished); QString error;
        const auto id=jobs.submit(FileJobService::Kind::Rename,root,"Models/Other/model.safetensors","renamed.safetensors",&error);
        CTEST_CHECK_MESSAGE(!id.isEmpty(),qPrintable(error)); CTEST_WAIT_COMPARE_TIMEOUT(finished.size(),1,30000);
        CTEST_COMPARE(finished[0][0].toString(),id); CTEST_CHECK_MESSAGE(finished[0][2].toString().isEmpty(),qPrintable(finished[0][2].toString()));
        CTEST_CHECK(QFileInfo::exists(models+"/renamed.safetensors"));
        CTEST_CHECK(!QFileInfo::exists(models+"/renamed.safetensors.iisacc.json"));
        QFile renamedMetadata(root+"/.society-runtime/classifications/models/Other/renamed.safetensors.iisacc.json");
        CTEST_CHECK(renamedMetadata.open(QIODevice::ReadOnly));
        CTEST_COMPARE(QJsonDocument::fromJson(renamedMetadata.readAll()).object().value("metadata").toObject().value("title").toString(),QStringLiteral("Original model"));
        CTEST_CHECK(!QFileInfo::exists(file.fileName())); CTEST_COMPARE(recorded.size(),1);
        ContainerJobStorage storage(std::filesystem::path(fixture.filePath("jobs").toStdU16String()));
        auto store=storage.open(false); CTEST_CHECK(store); const auto saved=store->job(id.toStdString()); CTEST_CHECK(saved);
        CTEST_COMPARE(saved->state,ii::job::JobState::succeeded);
        society_ctest::SignalRecorder inspected(&jobs,&FileJobService::inspected);
        jobs.inspect("ticket",id); CTEST_WAIT_COMPARE_TIMEOUT(inspected.size(),1,10000);
        CTEST_COMPARE(inspected[0][0].toString(),QStringLiteral("ticket"));
        CTEST_CHECK(inspected[0][2].toString().isEmpty());
        const auto state=inspected[0][1].toMap();
        CTEST_COMPARE(state.value("state").toString(),QStringLiteral("succeeded"));
        CTEST_COMPARE(state.value("path").toString(),QStringLiteral("Models/Other/renamed.safetensors"));
    }
    void rejectsRelatedRequestsButNotIndependentRequests() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-XXXXXX"); CTEST_CHECK(fixture.isValid());
        const auto root=fixture.filePath("drive.societycontainer"); CTEST_CHECK(QDir().mkdir(root)); CTEST_CHECK(SocietyDrive::create(root));
        for (const auto &name : {"a.txt","b.txt"}) { QFile f(root+"/Files/"+name); CTEST_CHECK(f.open(QIODevice::WriteOnly)); f.write("a"); }
        FileJobService jobs(fixture.filePath("jobs")); society_ctest::SignalRecorder done(&jobs,&FileJobService::finished); QString error;
        const auto first=jobs.submit(FileJobService::Kind::Trash,root,"Files/a.txt",{},&error); CTEST_CHECK_MESSAGE(!first.isEmpty(),qPrintable(error));
        CTEST_CHECK(jobs.submit(FileJobService::Kind::Trash,root,"Files/a.txt",{},&error).isEmpty());
        const auto second=jobs.submit(FileJobService::Kind::Trash,root,"Files/b.txt",{},&error); CTEST_CHECK_MESSAGE(!second.isEmpty(),qPrintable(error));
        CTEST_WAIT_COMPARE_TIMEOUT(done.size(),2,30000);
        for (const auto &event:done) CTEST_CHECK_MESSAGE(event[2].toString().isEmpty(),qPrintable(event[2].toString()));
        CTEST_CHECK(!QFileInfo::exists(root+"/Files/a.txt")); CTEST_CHECK(!QFileInfo::exists(root+"/Files/b.txt"));
    }
    void protectsInternalMetadata() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-XXXXXX"); CTEST_CHECK(fixture.isValid());
        const auto root=fixture.filePath("drive.societycontainer"); CTEST_CHECK(QDir().mkdir(root)); CTEST_CHECK(SocietyDrive::create(root));
        QFile metadata(root+"/Files/a.txt.iisacc.json"); CTEST_CHECK(metadata.open(QIODevice::WriteOnly)); metadata.write("{}"); metadata.close();
        FileJobService jobs(fixture.filePath("jobs")); society_ctest::SignalRecorder done(&jobs,&FileJobService::finished); QString error;
        const auto id=jobs.submit(FileJobService::Kind::Trash,root,"Files/a.txt.iisacc.json",{},&error);
        CTEST_CHECK_MESSAGE(!id.isEmpty(),qPrintable(error)); CTEST_WAIT_COMPARE_TIMEOUT(done.size(),1,30000);
        CTEST_CHECK(!done[0][2].toString().isEmpty()); CTEST_CHECK(QFileInfo::exists(metadata.fileName()));
    }
    void interruptedOperationsRemainUnknownUntilVerified() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-XXXXXX"); CTEST_CHECK(fixture.isValid());
        ContainerJobStorage storage(std::filesystem::path(fixture.filePath("jobs").toStdU16String()));
        const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces); auto store=storage.open(true);
        auto lease=storage.tryLease(id.toStdString(),true); CTEST_CHECK(lease && lease->locked());
        store->create({id.toStdString(),"file-trash","local-user:unverified","drive","","node",false},
                      {{"item","Files/a.txt","","","a.txt"}});
        store->begin(id.toStdString(),"node","trash"); const auto claim=store->claim(id.toStdString(),"item");
        CTEST_CHECK(!claim.token.empty()); FileJobService jobs(fixture.filePath("jobs"));
        society_ctest::SignalRecorder inspected(&jobs,&FileJobService::inspected); jobs.inspect("live",id);
        CTEST_WAIT_COMPARE_TIMEOUT(inspected.size(),1,10000);
        CTEST_COMPARE(inspected[0][1].toMap().value("state").toString(),QStringLiteral("running"));
        lease.reset(); jobs.inspect("interrupted",id); CTEST_WAIT_COMPARE_TIMEOUT(inspected.size(),2,10000);
        const auto recovered=inspected[1][1].toMap();
        CTEST_COMPARE(recovered.value("state").toString(),QStringLiteral("attention"));
        CTEST_COMPARE(recovered.value("unknown").toULongLong(),qulonglong(1));
    }
    void partialResultsRetainTheirActualLocation() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-XXXXXX"); CTEST_CHECK(fixture.isValid());
        const auto root=fixture.filePath("drive.societycontainer"); CTEST_CHECK(QDir().mkdir(root)); CTEST_CHECK(SocietyDrive::create(root));
        QFile source(root+"/Files/original.txt"); CTEST_CHECK(source.open(QIODevice::WriteOnly)); source.write("data"); source.close();
        FileOperations::setRelocationHandler([](const QString &,const QString &,const QString &,FileOperations::RelocationPhase phase) {
            if (phase==FileOperations::RelocationPhase::Commit) throw std::runtime_error("registration interrupted");
        });
        auto reset=qScopeGuard([]{FileOperations::setRelocationHandler({});});
        FileJobService jobs(fixture.filePath("jobs")); society_ctest::SignalRecorder done(&jobs,&FileJobService::finished); QString error;
        const auto id=jobs.submit(FileJobService::Kind::Rename,root,"Files/original.txt","renamed.txt",&error);
        CTEST_CHECK_MESSAGE(!id.isEmpty(),qPrintable(error)); CTEST_WAIT_COMPARE_TIMEOUT(done.size(),1,30000);
        CTEST_CHECK(!done[0][2].toString().isEmpty()); CTEST_COMPARE(done[0][1].toString(),QStringLiteral("Files/renamed.txt"));
        society_ctest::SignalRecorder inspected(&jobs,&FileJobService::inspected); jobs.inspect("partial",id);
        CTEST_WAIT_COMPARE_TIMEOUT(inspected.size(),1,10000);
        const auto state=inspected[0][1].toMap(); CTEST_COMPARE(state.value("state").toString(),QStringLiteral("failed"));
        CTEST_COMPARE(state.value("path").toString(),QStringLiteral("Files/renamed.txt"));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name" << std::endl; return 1; }
    society_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    NativeFileJobsTests tests;
    runner.run("independentCopiesWaitForTheContainerCommitLock", [&] { tests.independentCopiesWaitForTheContainerCommitLock(); });
    runner.run("serviceStopInterruptsAWaitingCopyWithoutPublishingIt", [&] { tests.serviceStopInterruptsAWaitingCopyWithoutPublishingIt(); });
    runner.run("invalidCommitLockFailsWithoutWaitingIndefinitely", [&] { tests.invalidCommitLockFailsWithoutWaitingIndefinitely(); });
    runner.run("copiesExternalAssetAndCompanionsAsDurableJob", [&] { tests.copiesExternalAssetAndCompanionsAsDurableJob(); });
    runner.run("renamePersistsAndMovesCompanions", [&] { tests.renamePersistsAndMovesCompanions(); });
    runner.run("rejectsRelatedRequestsButNotIndependentRequests", [&] { tests.rejectsRelatedRequestsButNotIndependentRequests(); });
    runner.run("protectsInternalMetadata", [&] { tests.protectsInternalMetadata(); });
    runner.run("interruptedOperationsRemainUnknownUntilVerified", [&] { tests.interruptedOperationsRemainUnknownUntilVerified(); });
    runner.run("partialResultsRetainTheirActualLocation", [&] { tests.partialResultsRetainTheirActualLocation(); });
    return runner.result();
}
