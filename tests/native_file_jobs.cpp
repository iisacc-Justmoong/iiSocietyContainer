#include "FileJobService.h"
#include "ContainerJobStorage.h"
#include "SocietyDrive.h"
#include "FileOperations.h"
#include <FileCatalog.h>
#include <TemporaryLease.h>
#include <Job.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace iiSocietyContainer;
class NativeFileJobsTests final : public QObject {
    Q_OBJECT
private slots:
    void copiesExternalAssetAndCompanionsAsDurableJob() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-XXXXXX"); QVERIFY(fixture.isValid());
        const auto root=fixture.filePath("drive.societycontainer"); QVERIFY(QDir().mkdir(root)); QVERIFY(SocietyDrive::create(root));
        const auto outside=fixture.filePath("outside"); QVERIFY(QDir().mkdir(outside));
        QFile source(outside+"/asset.txt"); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("original"); source.close();
        iiFileProvider::FileCatalog catalog(std::filesystem::path(outside.toStdU16String()));
        QVERIFY(catalog.registerFile("asset.txt").metadataWritten);
        QFile metadata(source.fileName()+".iisacc.json"); QVERIFY(QFileInfo::exists(metadata.fileName()));
        FileJobService jobs(fixture.filePath("jobs")); QSignalSpy done(&jobs,&FileJobService::finished); QString error;
        const auto canonical=QFileInfo(source).canonicalFilePath();
        const auto id=jobs.submit(FileJobService::Kind::Copy,root,canonical,"Files",&error);
        QVERIFY2(!id.isEmpty(),qPrintable(error)); QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,120000);
        QVERIFY2(done[0][2].toString().isEmpty(),qPrintable(done[0][2].toString()));
        QCOMPARE(done[0][1].toString(),QStringLiteral("Files/asset.txt"));
        QVERIFY(QFileInfo::exists(source.fileName())); QVERIFY(QFileInfo::exists(metadata.fileName()));
        QVERIFY(QFileInfo::exists(root+"/Files/asset.txt")); QVERIFY(QFileInfo::exists(root+"/Files/asset.txt.iisacc.json"));
        QSignalSpy inspected(&jobs,&FileJobService::inspected); jobs.inspect("copy",id);
        QTRY_COMPARE_WITH_TIMEOUT(inspected.size(),1,10000);
        QCOMPARE(inspected[0][1].toMap().value("state").toString(),QStringLiteral("succeeded"));
        QVERIFY(jobs.submit(FileJobService::Kind::Copy,root,canonical,"Files/../Models",&error).isEmpty());
        QVERIFY(jobs.submit(FileJobService::Kind::Copy,root,"relative.txt","Files",&error).isEmpty());
        const auto protectedId=jobs.submit(FileJobService::Kind::Copy,root,QFileInfo(metadata).canonicalFilePath(),"Files",&error);
        QVERIFY2(!protectedId.isEmpty(),qPrintable(error)); QTRY_COMPARE_WITH_TIMEOUT(done.size(),2,30000);
        QVERIFY(!done[1][2].toString().isEmpty());
    }
    void renamePersistsAndMovesCompanions() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-XXXXXX"); QVERIFY(fixture.isValid());
        const auto root=fixture.filePath("drive.societycontainer"); QVERIFY(QDir().mkdir(root));
        const auto drive=SocietyDrive::create(root); QVERIFY(drive);
        const auto models=root+"/Models/Other"; QVERIFY(QDir().mkpath(models));
        QFile file(models+"/model.safetensors"); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture"); file.close();
        iiFileProvider::FileCatalog catalog(std::filesystem::path((root+"/Models").toStdU16String()));
        QVERIFY(catalog.registerFile("Other/model.safetensors").metadataWritten);
        FileJobService jobs(fixture.filePath("jobs")); QSignalSpy recorded(&jobs,&FileJobService::recorded);
        QSignalSpy finished(&jobs,&FileJobService::finished); QString error;
        const auto id=jobs.submit(FileJobService::Kind::Rename,root,"Models/Other/model.safetensors","renamed.safetensors",&error);
        QVERIFY2(!id.isEmpty(),qPrintable(error)); QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,30000);
        QCOMPARE(finished[0][0].toString(),id); QVERIFY2(finished[0][2].toString().isEmpty(),qPrintable(finished[0][2].toString()));
        QVERIFY(QFileInfo::exists(models+"/renamed.safetensors"));
        QVERIFY(QFileInfo::exists(models+"/renamed.safetensors.iisacc.json"));
        QVERIFY(!QFileInfo::exists(file.fileName())); QCOMPARE(recorded.size(),1);
        ContainerJobStorage storage(std::filesystem::path(fixture.filePath("jobs").toStdU16String()));
        auto store=storage.open(false); QVERIFY(store); const auto saved=store->job(id.toStdString()); QVERIFY(saved);
        QCOMPARE(saved->state,ii::job::JobState::succeeded);
        QSignalSpy inspected(&jobs,&FileJobService::inspected);
        jobs.inspect("ticket",id); QTRY_COMPARE_WITH_TIMEOUT(inspected.size(),1,10000);
        QCOMPARE(inspected[0][0].toString(),QStringLiteral("ticket"));
        QVERIFY(inspected[0][2].toString().isEmpty());
        const auto state=inspected[0][1].toMap();
        QCOMPARE(state.value("state").toString(),QStringLiteral("succeeded"));
        QCOMPARE(state.value("path").toString(),QStringLiteral("Models/Other/renamed.safetensors"));
    }
    void rejectsRelatedRequestsButNotIndependentRequests() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-XXXXXX"); QVERIFY(fixture.isValid());
        const auto root=fixture.filePath("drive.societycontainer"); QVERIFY(QDir().mkdir(root)); QVERIFY(SocietyDrive::create(root));
        for (const auto &name : {"a.txt","b.txt"}) { QFile f(root+"/Files/"+name); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("a"); }
        FileJobService jobs(fixture.filePath("jobs")); QSignalSpy done(&jobs,&FileJobService::finished); QString error;
        const auto first=jobs.submit(FileJobService::Kind::Trash,root,"Files/a.txt",{},&error); QVERIFY2(!first.isEmpty(),qPrintable(error));
        QVERIFY(jobs.submit(FileJobService::Kind::Trash,root,"Files/a.txt",{},&error).isEmpty());
        const auto second=jobs.submit(FileJobService::Kind::Trash,root,"Files/b.txt",{},&error); QVERIFY2(!second.isEmpty(),qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),2,30000);
        for (const auto &event:done) QVERIFY2(event[2].toString().isEmpty(),qPrintable(event[2].toString()));
        QVERIFY(!QFileInfo::exists(root+"/Files/a.txt")); QVERIFY(!QFileInfo::exists(root+"/Files/b.txt"));
    }
    void protectsInternalMetadata() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-XXXXXX"); QVERIFY(fixture.isValid());
        const auto root=fixture.filePath("drive.societycontainer"); QVERIFY(QDir().mkdir(root)); QVERIFY(SocietyDrive::create(root));
        QFile metadata(root+"/Files/a.txt.iisacc.json"); QVERIFY(metadata.open(QIODevice::WriteOnly)); metadata.write("{}"); metadata.close();
        FileJobService jobs(fixture.filePath("jobs")); QSignalSpy done(&jobs,&FileJobService::finished); QString error;
        const auto id=jobs.submit(FileJobService::Kind::Trash,root,"Files/a.txt.iisacc.json",{},&error);
        QVERIFY2(!id.isEmpty(),qPrintable(error)); QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,30000);
        QVERIFY(!done[0][2].toString().isEmpty()); QVERIFY(QFileInfo::exists(metadata.fileName()));
    }
    void interruptedOperationsRemainUnknownUntilVerified() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-XXXXXX"); QVERIFY(fixture.isValid());
        ContainerJobStorage storage(std::filesystem::path(fixture.filePath("jobs").toStdU16String()));
        const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces); auto store=storage.open(true);
        auto lease=storage.tryLease(id.toStdString(),true); QVERIFY(lease && lease->locked());
        store->create({id.toStdString(),"file-trash","local-user:unverified","drive","","node",false},
                      {{"item","Files/a.txt","","","a.txt"}});
        store->begin(id.toStdString(),"node","trash"); const auto claim=store->claim(id.toStdString(),"item");
        QVERIFY(!claim.token.empty()); FileJobService jobs(fixture.filePath("jobs"));
        QSignalSpy inspected(&jobs,&FileJobService::inspected); jobs.inspect("live",id);
        QTRY_COMPARE_WITH_TIMEOUT(inspected.size(),1,10000);
        QCOMPARE(inspected[0][1].toMap().value("state").toString(),QStringLiteral("running"));
        lease.reset(); jobs.inspect("interrupted",id); QTRY_COMPARE_WITH_TIMEOUT(inspected.size(),2,10000);
        const auto recovered=inspected[1][1].toMap();
        QCOMPARE(recovered.value("state").toString(),QStringLiteral("attention"));
        QCOMPARE(recovered.value("unknown").toULongLong(),qulonglong(1));
    }
    void partialResultsRetainTheirActualLocation() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_RESOURCES) + "/file-jobs-XXXXXX"); QVERIFY(fixture.isValid());
        const auto root=fixture.filePath("drive.societycontainer"); QVERIFY(QDir().mkdir(root)); QVERIFY(SocietyDrive::create(root));
        QFile source(root+"/Files/original.txt"); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("data"); source.close();
        FileOperations::setRelocationHandler([](const QString &,const QString &,const QString &,FileOperations::RelocationPhase phase) {
            if (phase==FileOperations::RelocationPhase::Commit) throw std::runtime_error("registration interrupted");
        });
        auto reset=qScopeGuard([]{FileOperations::setRelocationHandler({});});
        FileJobService jobs(fixture.filePath("jobs")); QSignalSpy done(&jobs,&FileJobService::finished); QString error;
        const auto id=jobs.submit(FileJobService::Kind::Rename,root,"Files/original.txt","renamed.txt",&error);
        QVERIFY2(!id.isEmpty(),qPrintable(error)); QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,30000);
        QVERIFY(!done[0][2].toString().isEmpty()); QCOMPARE(done[0][1].toString(),QStringLiteral("Files/renamed.txt"));
        QSignalSpy inspected(&jobs,&FileJobService::inspected); jobs.inspect("partial",id);
        QTRY_COMPARE_WITH_TIMEOUT(inspected.size(),1,10000);
        const auto state=inspected[0][1].toMap(); QCOMPARE(state.value("state").toString(),QStringLiteral("failed"));
        QCOMPARE(state.value("path").toString(),QStringLiteral("Files/renamed.txt"));
    }
};
QTEST_GUILESS_MAIN(NativeFileJobsTests)
#include "native_file_jobs.moc"
