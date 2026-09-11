#include <SocietyDrive.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtCore/qscopeguard.h>
#include <QtTest/QTest>

#include <memory>

using namespace iiSocietyContainer;

class SocietyDriveTests : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        workspace = std::make_unique<QTemporaryDir>(QDir::current().filePath("society-drive-XXXXXX"));
        QVERIFY(workspace->isValid());
    }

    void createsAndReopensDriveWithEightIndependentRoots()
    {
        QString error;
        const auto drive = SocietyDrive::create(workspace->path(), &error);
        QVERIFY2(drive.has_value(), qPrintable(error));
        QVERIFY(error.isEmpty());
        QVERIFY(drive->isValid());
        QVERIFY(!drive->identifier().isEmpty());
        QCOMPARE(drive->displayName(), QStringLiteral("Society"));
        QCOMPARE(drive->sections(), allStoreSections());
        for (const auto section : drive->sections()) {
            const auto path = drive->sectionPath(section);
            QVERIFY(QFileInfo(path).isDir());
            QCOMPARE(QFileInfo(path).fileName(), storeSectionName(section));
            QCOMPARE(drive->sectionForPath(path).value(), section);
            QCOMPARE(drive->sectionForPath(storeSectionName(section)).value(), section);
        }
        const auto reopened = SocietyDrive::open(workspace->path(), &error);
        QVERIFY2(reopened.has_value(), qPrintable(error));
        QCOMPARE(reopened->identifier(), drive->identifier());
        const auto repeated = SocietyDrive::create(workspace->path(), &error);
        QVERIFY(repeated.has_value());
        QCOMPARE(repeated->identifier(), drive->identifier());
    }

    void legacyNameKeepsIdentityAndSourceData()
    {
        const auto created = SocietyDrive::create(workspace->path());
        QVERIFY(created);
        QFile manifest(workspace->filePath(".society-drive.json"));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        auto data = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
        QCOMPARE(data.value("displayName").toString(), QString("Society"));
        data["displayName"] = "Society Container";
        const auto legacy = QJsonDocument(data).toJson();
        QVERIFY(manifest.open(QIODevice::WriteOnly)); QCOMPARE(manifest.write(legacy), legacy.size()); manifest.close();
        QFile file(workspace->filePath("Files/keep.txt")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("keep"); file.close();
        const auto reopened = SocietyDrive::open(workspace->path());
        QVERIFY(reopened); QCOMPARE(reopened->identifier(), created->identifier());
        QCOMPARE(reopened->displayName(), QString("Society"));
        QVERIFY(SocietyDrive::create(workspace->path()));
        QVERIFY(manifest.open(QIODevice::ReadOnly)); QCOMPARE(manifest.readAll(), legacy); manifest.close();
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("keep"));
        data["displayName"] = "Unrelated drive";
        QVERIFY(manifest.open(QIODevice::WriteOnly)); manifest.write(QJsonDocument(data).toJson()); manifest.close();
        QVERIFY(!SocietyDrive::open(workspace->path()));
    }

    void adoptsHostIdentityWithoutMovingSectionsOrLosingData()
    {
        const auto original = SocietyDrive::create(workspace->path()); QVERIFY(original);
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QFile file(workspace->filePath("Files/keep.txt")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("keep"); file.close();
        QString error;
        QVERIFY(!SocietyDrive::adoptReplicaIdentity(workspace->path(), original->identifier(), "invalid", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!SocietyDrive::adoptReplicaIdentity(workspace->path(), "wrong", host, &error));
        QVERIFY(original->isValid());
        const auto mirror = SocietyDrive::adoptReplicaIdentity(workspace->path(), original->identifier(), host, &error);
        QVERIFY2(mirror, qPrintable(error)); QCOMPARE(mirror->identifier(), host);
        QCOMPARE(mirror->rootPath(), original->rootPath());
        QVERIFY(!original->isValid()); QVERIFY(mirror->isValid());
        QCOMPARE(SocietyDrive::create(workspace->path())->identifier(), host);
        QVERIFY(SocietyDrive::adoptReplicaIdentity(workspace->path(), original->identifier(), host));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("keep"));
        QCOMPARE(mirror->sections(), allStoreSections());
        QVERIFY(!SocietyDrive::completeReplica(workspace->path(), original->identifier(), &error));
        QVERIFY(SocietyDrive::completeReplica(workspace->path(), host, &error));
        QFile manifest(workspace->filePath(".society-drive.json")); QVERIFY(manifest.open(QIODevice::ReadOnly));
        const auto data = QJsonDocument::fromJson(manifest.readAll()).object();
        QCOMPARE(data.value("localIdentifier").toString(), original->identifier());
        QVERIFY(data.value("replicaReady").toBool());
        const auto repeatedAdoption = SocietyDrive::adoptReplicaIdentity(workspace->path(), host, host);
        QVERIFY(repeatedAdoption); QVERIFY(!repeatedAdoption->isReady());
        QVERIFY(SocietyDrive::completeReplica(workspace->path(), host));
    }

    void keepsExistingContentAndRejectsDirectoryConflictsBeforeCreation()
    {
        QFile conflict(workspace->filePath("Models"));
        QVERIFY(conflict.open(QIODevice::WriteOnly));
        QCOMPARE(conflict.write("original"), qint64(8));
        conflict.close();
        QString error;
        QVERIFY(!SocietyDrive::create(workspace->path(), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(workspace->filePath("Asset Library")));
        QVERIFY(!QFileInfo::exists(workspace->filePath(".society-drive.json")));
        QVERIFY(conflict.open(QIODevice::ReadOnly));
        QCOMPARE(conflict.readAll(), QByteArray("original"));
    }

    void preservesUnassignedFiles()
    {
        QFile file(workspace->filePath("existing.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("original"), qint64(8));
        file.close();
        const auto drive = SocietyDrive::create(workspace->path());
        QVERIFY(drive.has_value());
        QVERIFY(!drive->sectionForPath(file.fileName()));
        QVERIFY(!drive->sectionForPath("."));
        QVERIFY(!drive->sectionForPath("../"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("original"));
    }

    void rejectsNestedContainersBeforeWriting()
    {
        const auto drive = SocietyDrive::create(workspace->path());
        QVERIFY(drive);
        const auto files = drive->sectionPath(StoreSection::Files);
        QString error;
        QVERIFY(!SocietyDrive::create(files, &error));
        QVERIFY(error.contains("source"));
        QVERIFY(QDir(files).isEmpty());
        QVERIFY(QDir().mkpath(QDir(files).filePath("Nested/Deep")));
        QVERIFY(!SocietyDrive::create(QDir(files).filePath("Nested/Deep"), &error));
        QVERIFY(QDir(QDir(files).filePath("Nested/Deep")).isEmpty());

        // Reproduce a container written by an older app inside the public area.
        for (const auto section : allStoreSections())
            QVERIFY(QDir().mkdir(QDir(files).filePath(storeSectionName(section))));
        QVERIFY(QFile::copy(workspace->filePath(".society-drive.json"), QDir(files).filePath(".society-drive.json")));
        QVERIFY(!SocietyDrive::open(files, &error));
        QVERIFY(!SocietyDrive::create(files, &error));
        QVERIFY(QFileInfo::exists(QDir(files).filePath(".society-drive.json")));
#ifdef Q_OS_UNIX
        const auto alias = workspace->filePath("alias");
        QVERIFY(QFile::link(files, alias));
        QVERIFY(!SocietyDrive::open(alias, &error));
#endif
        QVERIFY(drive->isValid());
    }

    void rejectsFinderReplicasAndTheirDescendants()
    {
#ifdef Q_OS_MACOS
        const auto cloud = workspace->filePath("Library/CloudStorage");
        const auto replica = QDir(cloud).filePath("SocietyContainer-SocietyContainer");
        QVERIFY(QDir().mkpath(replica));
        // An existing misplaced manifest must be rejected as well as new creation.
        QVERIFY(SocietyDrive::create(replica));
        const auto homeBefore = qgetenv("HOME");
        const auto restoreHome = qScopeGuard([homeBefore] { qputenv("HOME", homeBefore); });
        qputenv("HOME", workspace->path().toUtf8());
        QCOMPARE(QDir::homePath(), workspace->path());
        QString error;
        QVERIFY(!SocietyDrive::open(replica, &error));
        QVERIFY(error.contains("Finder"));
        QVERIFY(!SocietyDrive::create(replica, &error));
        const auto child = QDir(replica).filePath("New Folder");
        QVERIFY(QDir().mkdir(child));
        QVERIFY(!SocietyDrive::create(child, &error));
        QVERIFY(QDir(child).isEmpty());
        const auto alias = workspace->filePath("replica-alias");
        QVERIFY(QFile::link(replica, alias));
        QVERIFY(!SocietyDrive::open(alias, &error));
        const auto sibling = workspace->filePath("Library/CloudStorage-backup");
        QVERIFY(QDir().mkdir(sibling));
        QVERIFY(SocietyDrive::create(sibling, &error));
#else
        QSKIP("Finder replicas are a macOS source-location boundary.");
#endif
    }

    void doesNotInferAnUninitializedDrive()
    {
        QString error;
        QVERIFY(!SocietyDrive::open(workspace->path(), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!SocietyDrive::create(QString(), &error));
        QVERIFY(!SocietyDrive::create(workspace->filePath("missing"), &error));
        QVERIFY(!QFileInfo::exists(workspace->filePath("missing")));
    }

    void rejectsInvalidManifestWithoutOverwritingIt()
    {
        QFile manifest(workspace->filePath(".society-drive.json"));
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        const QByteArray data("{\"type\":\"another-format\"}");
        QCOMPARE(manifest.write(data), data.size());
        manifest.close();
        QString error;
        QVERIFY(!SocietyDrive::create(workspace->path(), &error));
        QVERIFY(!SocietyDrive::open(workspace->path(), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        QCOMPARE(manifest.readAll(), data);
    }

    void detectsMissingSectionsAndInvalidSectionValues()
    {
        const auto drive = SocietyDrive::create(workspace->path());
        QVERIFY(drive.has_value());
        QVERIFY(drive->sectionPath(static_cast<StoreSection>(-1)).isEmpty());
        QVERIFY(QDir().rmdir(drive->sectionPath(StoreSection::Models)));
        QVERIFY(!drive->isValid());
        QVERIFY(!SocietyDrive::open(workspace->path()));
    }

    void preventsSectionsFromAliasingEachOther()
    {
#ifdef Q_OS_UNIX
        QVERIFY(QDir().mkdir(workspace->filePath("Files")));
        QVERIFY(QFile::link(workspace->filePath("Files"), workspace->filePath("Models")));
        QString error;
        QVERIFY(!SocietyDrive::create(workspace->path(), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(workspace->filePath("Asset Library")));
#else
        QSKIP("QFile::link creates shortcuts on Windows.");
#endif
    }

private:
    std::unique_ptr<QTemporaryDir> workspace;
};

QTEST_GUILESS_MAIN(SocietyDriveTests)
#include "society_drive.moc"
