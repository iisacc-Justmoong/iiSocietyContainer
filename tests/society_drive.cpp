#include "SymbolicLink.h"
#include <SocietyDrive.h>
#include <StorageMaintenance.h>
#include <iiFileProviderNative.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtCore/qscopeguard.h>
#include <QtTest/QTest>

#include <memory>
#include <filesystem>
#include <atomic>
#include <thread>

using namespace iiSocietyContainer;

class SocietyDriveTests : public QObject
{
    Q_OBJECT
private slots:
    void replicaIdentityUpdatesWhileMaintenanceReadsTheManifest()
    {
        QTemporaryDir fixture(QDir::current().filePath("replica-maintenance-XXXXXX.societycontainer"));
        QVERIFY(fixture.isValid());
        QString error;
        const auto drive = SocietyDrive::create(fixture.path(), &error);
        QVERIFY2(drive, qPrintable(error));
        const auto root = std::filesystem::path(fixture.path().toStdU16String());
        const auto identifier = drive->identifier().toStdString();
        std::atomic<unsigned> reads = 0;
        std::jthread maintenance([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                (void)StorageMaintenance::runOnce(root, identifier);
                ++reads;
            }
        });
        while (reads.load() == 0) std::this_thread::yield();
        for (int i = 0; i < 64; ++i) {
            // Keep a real provider snapshot open across replacement, matching
            // maintenance and presentation readers rather than only racing it.
            iiFileProvider::native::ReadFile snapshot(root / ".society-drive.json");
            QVERIFY2(SocietyDrive::adoptReplicaIdentity(fixture.path(), drive->identifier(), drive->identifier(), &error), qPrintable(error));
            QVERIFY2(SocietyDrive::completeReplica(fixture.path(), drive->identifier(), &error), qPrintable(error));
        }
        maintenance.request_stop();
        maintenance.join();
        QVERIFY(reads.load() > 0);
        QVERIFY(drive->isReady());
    }
    void nativeAndQtUnicodeParentsPreserveThePackageBoundary()
    {
        QTemporaryDir parent(QDir::current().filePath("unicode-package-XXXXXX")); QVERIFY(parent.isValid());
        const auto qtParent = parent.filePath(QString::fromUtf8("Qt 한글 #100%"));
        QVERIFY(QDir().mkdir(qtParent));
        const auto nativeParent = parent.filePath(QString::fromUtf8("Native 한글 #100%"));
#ifdef Q_OS_WIN
        QVERIFY(std::filesystem::create_directory(std::filesystem::path(nativeParent.toStdWString())));
#else
        QVERIFY(std::filesystem::create_directory(std::filesystem::path(nativeParent.toStdString())));
#endif
        for (const auto &location : {qtParent, nativeParent}) {
            QString error;
            const auto drive = SocietyDrive::createAt(location, &error);
            QVERIFY2(drive, qPrintable(error));
            QCOMPARE(drive->rootPath(), location + "/Society.societycontainer");
            const auto reopened = SocietyDrive::open(drive->rootPath(), &error);
            QVERIFY2(reopened, qPrintable(error));
            QCOMPARE(reopened->identifier(), drive->identifier());
            const auto unicodeFile = drive->resolvePath(QString::fromUtf8("Files/사용자 한글 #100%.txt"));
            QVERIFY(!unicodeFile.isEmpty());
            QFile file(unicodeFile); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("user content"); file.close();
            QCOMPARE(drive->resolvePath(QString::fromUtf8("Files/사용자 한글 #100%.txt")), unicodeFile);
            const auto alias = location + QString::fromUtf8("-별칭.societycontainer");
            QVERIFY(createTestSymbolicLink(drive->rootPath(), alias));
            QVERIFY(!SocietyDrive::open(alias));
            QVERIFY(!SocietyDrive::createAt(alias));
        }
    }
    void bareFoldersImagesAndRedirectedPackagesAreRejectedWithoutWrites()
    {
        QTemporaryDir parent(QDir::current().filePath("package-boundary-XXXXXX")); QVERIFY(parent.isValid());
        const auto drive = SocietyDrive::createAt(parent.path()); QVERIFY(drive);
        const auto bare = parent.filePath("Society"); QVERIFY(QDir().mkdir(bare));
        QFile userFile(bare + "/user.txt"); QVERIFY(userFile.open(QIODevice::WriteOnly));
        QCOMPARE(userFile.write("keep"), qint64(4)); userFile.close();
        QVERIFY(QFile::copy(drive->rootPath() + "/.society-drive.json", bare + "/.society-drive.json"));
        QString error;
        QVERIFY(!SocietyDrive::create(bare, &error)); QVERIFY(error.contains(".societycontainer"));
        QVERIFY(!SocietyDrive::open(bare, &error)); QVERIFY(!SocietyDrive::createAt(bare, &error));
        QCOMPARE(QDir(bare).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size(), 2);
        QVERIFY(userFile.open(QIODevice::ReadOnly)); QCOMPARE(userFile.readAll(), QByteArray("keep"));
        const auto old = parent.filePath("Old.societycontainer"); QVERIFY(QDir().mkpath(old + "/bands"));
        QFile marker(old + "/Info.plist"); QVERIFY(marker.open(QIODevice::WriteOnly)); marker.write("legacy image"); marker.close();
        QVERIFY(!SocietyDrive::create(old)); QVERIFY(!SocietyDrive::open(old));
        QVERIFY(!QFileInfo::exists(old + "/.society-drive.json"));
        const auto alias = parent.filePath("Alias.societycontainer");
        QVERIFY(createTestSymbolicLink(drive->rootPath(), alias));
        QVERIFY(!SocietyDrive::create(alias)); QVERIFY(!SocietyDrive::open(alias));
        QVERIFY(drive->isValid());
    }
    void createsPackageStorageTreeAndReusesIdentity()
    {
        workspace = std::make_unique<QTemporaryDir>(QDir::current().filePath("drive-parent-XXXXXX"));
        QVERIFY(workspace->isValid());
        QString error;
        const auto drive = SocietyDrive::createAt(workspace->path(), &error);
        QVERIFY2(drive, qPrintable(error));
        QCOMPARE(drive->rootPath(), workspace->filePath("Society.societycontainer"));
        QVERIFY(!QFileInfo::exists(workspace->filePath("Society")));
        QVERIFY(!QFileInfo::exists(drive->rootPath() + "/bands"));
        for (const auto section : allStoreSections())
            QCOMPARE(drive->sectionPath(section), drive->rootPath() + '/' + storeSectionName(section));
        QVERIFY(QDir(drive->sectionPath(StoreSection::Models)).isEmpty());
        QCOMPARE(SocietyDrive::createAt(workspace->path())->identifier(), drive->identifier());
        QCOMPARE(SocietyDrive::createAt(drive->rootPath())->identifier(), drive->identifier());
        QVERIFY(QDir(drive->sectionPath(StoreSection::Models)).isEmpty());
        QVERIFY(!SocietyDrive::createAt(drive->sectionPath(StoreSection::Files), &error));
        QVERIFY(!QFileInfo::exists(drive->sectionPath(StoreSection::Files) + "/Society.societycontainer"));
    }
    void refusesConflictingOrRedirectedStorageFolder()
    {
        workspace = std::make_unique<QTemporaryDir>(QDir::current().filePath("drive-parent-XXXXXX"));
        QVERIFY(workspace->isValid());
        QFile conflict(workspace->filePath("Society.societycontainer"));
        QVERIFY(conflict.open(QIODevice::WriteOnly)); conflict.write("keep"); conflict.close();
        QVERIFY(!SocietyDrive::createAt(workspace->path()));
        QVERIFY(conflict.open(QIODevice::ReadOnly)); QCOMPARE(conflict.readAll(), QByteArray("keep")); conflict.close();
        QVERIFY(conflict.remove());
        QVERIFY(QDir().mkdir(workspace->filePath("outside")));
        QVERIFY(QFile::link(workspace->filePath("outside"), workspace->filePath("Society.societycontainer")));
        QVERIFY(!SocietyDrive::createAt(workspace->path()));
        QVERIFY(!QFileInfo::exists(workspace->filePath("outside/.society-drive.json")));
    }
    void accessesLogicalSectionTreesAndRejectsReplacedIdentity()
    {
        const auto drive = SocietyDrive::create(workspace->path()); QVERIFY(drive);
        QVERIFY(QDir().mkpath(workspace->filePath("Files/projects/empty")));
        QFile file(workspace->filePath("Files/projects/note.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("tree bytes"); file.close();
        auto snapshot = drive->tree(); QVERIFY(snapshot);
        QCOMPARE(snapshot.value->children.size(), std::size_t(9));
        const auto* note = snapshot.value->find("Files/projects/note.txt"); QVERIFY(note);
        QCOMPARE(note->size, std::uintmax_t(10));
        QVERIFY(note->parentPath == "Files/projects");
        QVERIFY(note->path == std::filesystem::path(file.fileName().toStdString()));
        QVERIFY(!snapshot.value->find(".society-drive.json"));
        const auto subtree = drive->tree("Files/projects"); QVERIFY(subtree);
        QVERIFY(subtree.value->relativePath == "Files/projects");
        QVERIFY(subtree.value->find("Files/projects/empty")->childrenLoaded);
        auto children = drive->entries("Files/projects"); QVERIFY(children);
        QCOMPARE(children.value->size(), std::size_t(2));
        QVERIFY(drive->entry("Files/projects/note.txt"));
        QVERIFY(!drive->entries("Files/projects/note.txt"));
        QVERIFY(!drive->tree("Files/projects/../../Models"));
        QVERIFY(!drive->tree(".society-sync"));
        QVERIFY(!drive->tree({}, {false, 64, 9}));
        QVERIFY(drive->tree({}, {false, 1, 10}));
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto replica = SocietyDrive::adoptReplicaIdentity(workspace->path(), drive->identifier(), host);
        QVERIFY(replica); QVERIFY(!drive->tree()); QVERIFY(!replica->tree());
        QVERIFY(drive->resolvePath("Files/new.txt").isEmpty());
        QVERIFY(drive->relativePath(file.fileName()).isEmpty());
        QCOMPARE(replica->resolvePath("Files/new/deep.txt"), workspace->filePath("Files/new/deep.txt"));
        QVERIFY(createTestSymbolicLink(workspace->filePath("Models"), workspace->filePath("Files/redirect")));
        QVERIFY(replica->resolvePath("Files/redirect/escape").isEmpty());
        QVERIFY(removeTestSymbolicLink(workspace->filePath("Files/redirect")));
        QVERIFY(replica->resolvePath("Files/projects/note.txt/child").isEmpty());
        QVERIFY(replica->resolvePath("Files/alternate:stream").isEmpty());
        QVERIFY(SocietyDrive::completeReplica(workspace->path(), host));
        QVERIFY(replica->tree());
    }

    void init()
    {
        workspace = std::make_unique<QTemporaryDir>(QDir::current().filePath("society-drive-XXXXXX.societycontainer"));
        QVERIFY(workspace->isValid());
    }

    void createsAndReopensDriveWithNineIndependentRoots()
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

    void migratesLegacyPhotosWithPreviewsAndIdentity()
    {
        const auto drive = SocietyDrive::create(workspace->path()); QVERIFY(drive);
        QFile manifest(workspace->filePath(".society-drive.json"));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        auto data = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
        QJsonArray legacySections;
        for (const auto &section : data.value("sections").toArray())
            if (section.toObject().value("id") != "photos") legacySections.append(section);
        data["sections"] = legacySections;
        QVERIFY(manifest.open(QIODevice::WriteOnly)); manifest.write(QJsonDocument(data).toJson()); manifest.close();
        QDir().rmdir(workspace->filePath("Photos"));
        QVERIFY(QDir().mkpath(workspace->filePath("Files/Photos/.previews")));
        const QStringList names{"image.societyphoto", ".previews/image.jpg", "clip.mp4"};
        for (const auto &name : names) {
            QFile file(workspace->filePath("Files/Photos/" + name));
            QVERIFY(file.open(QIODevice::WriteOnly)); file.write(name.toUtf8());
        }
        QString error;
        const auto upgraded = SocietyDrive::open(workspace->path(), &error); QVERIFY2(upgraded, qPrintable(error));
        QCOMPARE(upgraded->identifier(), drive->identifier());
        QVERIFY(!QFileInfo::exists(workspace->filePath("Files/Photos")));
        for (const auto &name : names) {
            QFile file(workspace->filePath("Photos/" + name));
            QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), name.toUtf8());
        }
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        const auto migrated = manifest.readAll(); manifest.close();
        QCOMPARE(QJsonDocument::fromJson(migrated).object().value("sections").toArray().size(), 9);
        QVERIFY(SocietyDrive::open(workspace->path()));
        QVERIFY(manifest.open(QIODevice::ReadOnly)); QCOMPARE(manifest.readAll(), migrated);
    }

    void legacyPhotosMergePreservesConflictsAndReplicaState_data()
    {
        QTest::addColumn<bool>("conflict");
        QTest::newRow("identical-duplicate") << false;
        QTest::newRow("conflicting-content") << true;
    }
    void legacyPhotosMergePreservesConflictsAndReplicaState()
    {
        QFETCH(bool, conflict);
        const auto drive = SocietyDrive::create(workspace->path()); QVERIFY(drive);
        QFile manifest(workspace->filePath(".society-drive.json"));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        auto data = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
        QJsonArray sections;
        for (const auto &entry : data.value("sections").toArray())
            if (entry.toObject().value("id") != "photos") sections.append(entry);
        data["sections"] = sections;
        data["localIdentifier"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        data["replicaReady"] = false;
        const auto oldManifest = QJsonDocument(data).toJson();
        QVERIFY(manifest.open(QIODevice::WriteOnly)); manifest.write(oldManifest); manifest.close();
        QVERIFY(QDir().mkpath(workspace->filePath("Files/Photos/.previews")));
        const auto write = [&](const QString &path, const QByteArray &bytes) {
            QFile file(workspace->filePath(path)); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        const auto read = [&](const QString &path) {
            QFile file(workspace->filePath(path)); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
        };
        QVERIFY(write("Files/Photos/a.societyphoto", "original"));
        QVERIFY(write("Files/Photos/.previews/a.jpg", "preview"));
        QVERIFY(write("Photos/a.societyphoto", conflict ? "different" : "original"));
        QVERIFY(write("Photos/new.jpg", "new"));
        QString error; const auto upgraded = SocietyDrive::open(workspace->path(), &error);
        if (conflict) {
            QVERIFY(!upgraded); QVERIFY(!error.isEmpty());
            QCOMPARE(read("Files/Photos/a.societyphoto"), "original");
            QCOMPARE(read("Files/Photos/.previews/a.jpg"), "preview");
            QCOMPARE(read("Photos/a.societyphoto"), "different");
            QCOMPARE(read(".society-drive.json"), oldManifest);
        } else {
            QVERIFY2(upgraded, qPrintable(error)); QVERIFY(!upgraded->isReady());
            QCOMPARE(upgraded->identifier(), drive->identifier());
            QVERIFY(!QFileInfo::exists(workspace->filePath("Files/Photos")));
            QCOMPARE(read("Photos/a.societyphoto"), "original");
            QCOMPARE(read("Photos/.previews/a.jpg"), "preview");
            const auto upgradedData = QJsonDocument::fromJson(read(".society-drive.json")).object();
            QCOMPARE(upgradedData.value("localIdentifier"), data.value("localIdentifier"));
            QCOMPARE(upgradedData.value("replicaReady"), QJsonValue(false));
        }
        QCOMPARE(read("Photos/new.jpg"), "new");
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
        manifest.close();
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
        QCOMPARE(QDir(files).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        QVERIFY(!QFileInfo::exists(QDir(files).filePath(".society-drive.json")));
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
        QVERIFY(createTestSymbolicLink(files, alias));
        QVERIFY(!SocietyDrive::open(alias, &error));
#endif
        QVERIFY(drive->isValid());
    }

    void rejectsFinderReplicasAndTheirDescendants()
    {
        workspace = std::make_unique<QTemporaryDir>(QDir::current().filePath("drive-parent-XXXXXX"));
        QVERIFY(workspace->isValid());
#ifdef Q_OS_MACOS
        const auto cloud = workspace->filePath("Library/CloudStorage");
        const auto replica = QDir(cloud).filePath("SocietyContainer-SocietyContainer.societycontainer");
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
        QVERIFY(createTestSymbolicLink(replica, alias));
        QVERIFY(!SocietyDrive::open(alias, &error));
        const auto sibling = workspace->filePath("Library/CloudStorage-backup.societycontainer");
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
        QVERIFY(QDir().rename(drive->sectionPath(StoreSection::Models), workspace->filePath("Models-original")));
        QVERIFY(!drive->isValid());
        QVERIFY(!SocietyDrive::open(workspace->path()));
    }

    void preventsSectionsFromAliasingEachOther()
    {
#ifdef Q_OS_UNIX
        QVERIFY(QDir().mkdir(workspace->filePath("Files")));
        QVERIFY(createTestSymbolicLink(workspace->filePath("Files"), workspace->filePath("Models")));
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
