#include "SymbolicLink.h"
#include <SocietyDrive.h>
#include <ModelType.h>
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
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include <QEvent>

#include <memory>
#include <filesystem>
#include <atomic>
#include <thread>

using namespace iiSocietyContainer;

class SocietyDriveTests : public QObject
{
public:
    void replicaIdentityUpdatesWhileMaintenanceReadsTheManifest()
    {
        QTemporaryDir fixture(QDir::current().filePath("replica-maintenance-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        QString error;
        const auto drive = SocietyDrive::create(fixture.path(), &error);
        CTEST_CHECK_MESSAGE(drive, qPrintable(error));
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
            CTEST_CHECK_MESSAGE(SocietyDrive::adoptReplicaIdentity(fixture.path(), drive->identifier(), drive->identifier(), &error), qPrintable(error));
            CTEST_CHECK_MESSAGE(SocietyDrive::completeReplica(fixture.path(), drive->identifier(), &error), qPrintable(error));
        }
        maintenance.request_stop();
        maintenance.join();
        CTEST_CHECK(reads.load() > 0);
        CTEST_CHECK(drive->isReady());
    }
    void nativeAndQtUnicodeParentsPreserveThePackageBoundary()
    {
        QTemporaryDir parent(QDir::current().filePath("unicode-package-XXXXXX")); CTEST_CHECK(parent.isValid());
        const auto qtParent = parent.filePath(QString::fromUtf8("Qt 한글 #100%"));
        CTEST_CHECK(QDir().mkdir(qtParent));
        const auto nativeParent = parent.filePath(QString::fromUtf8("Native 한글 #100%"));
#ifdef Q_OS_WIN
        CTEST_CHECK(std::filesystem::create_directory(std::filesystem::path(nativeParent.toStdWString())));
#else
        CTEST_CHECK(std::filesystem::create_directory(std::filesystem::path(nativeParent.toStdString())));
#endif
        for (const auto &location : {qtParent, nativeParent}) {
            QString error;
            const auto drive = SocietyDrive::createAt(location, &error);
            CTEST_CHECK_MESSAGE(drive, qPrintable(error));
            CTEST_COMPARE(drive->rootPath(), location + "/Society.societycontainer");
            const auto reopened = SocietyDrive::open(drive->rootPath(), &error);
            CTEST_CHECK_MESSAGE(reopened, qPrintable(error));
            CTEST_COMPARE(reopened->identifier(), drive->identifier());
            const auto unicodeFile = drive->resolvePath(QString::fromUtf8("Files/사용자 한글 #100%.txt"));
            CTEST_CHECK(!unicodeFile.isEmpty());
            QFile file(unicodeFile); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("user content"); file.close();
            CTEST_COMPARE(drive->resolvePath(QString::fromUtf8("Files/사용자 한글 #100%.txt")), unicodeFile);
            const auto alias = location + QString::fromUtf8("-별칭.societycontainer");
            CTEST_CHECK(createTestSymbolicLink(drive->rootPath(), alias));
            CTEST_CHECK(!SocietyDrive::open(alias));
            CTEST_CHECK(!SocietyDrive::createAt(alias));
        }
    }
    void bareFoldersImagesAndRedirectedPackagesAreRejectedWithoutWrites()
    {
        QTemporaryDir parent(QDir::current().filePath("package-boundary-XXXXXX")); CTEST_CHECK(parent.isValid());
        const auto drive = SocietyDrive::createAt(parent.path()); CTEST_CHECK(drive);
        const auto bare = parent.filePath("Society"); CTEST_CHECK(QDir().mkdir(bare));
        QFile userFile(bare + "/user.txt"); CTEST_CHECK(userFile.open(QIODevice::WriteOnly));
        CTEST_COMPARE(userFile.write("keep"), qint64(4)); userFile.close();
        CTEST_CHECK(QFile::copy(drive->rootPath() + "/.society-drive.json", bare + "/.society-drive.json"));
        QString error;
        CTEST_CHECK(!SocietyDrive::create(bare, &error)); CTEST_CHECK(error.contains(".societycontainer"));
        CTEST_CHECK(!SocietyDrive::open(bare, &error)); CTEST_CHECK(!SocietyDrive::createAt(bare, &error));
        CTEST_COMPARE(QDir(bare).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size(), 2);
        CTEST_CHECK(userFile.open(QIODevice::ReadOnly)); CTEST_COMPARE(userFile.readAll(), QByteArray("keep"));
        const auto old = parent.filePath("Old.societycontainer"); CTEST_CHECK(QDir().mkpath(old + "/bands"));
        QFile marker(old + "/Info.plist"); CTEST_CHECK(marker.open(QIODevice::WriteOnly)); marker.write("legacy image"); marker.close();
        CTEST_CHECK(!SocietyDrive::create(old)); CTEST_CHECK(!SocietyDrive::open(old));
        CTEST_CHECK(!QFileInfo::exists(old + "/.society-drive.json"));
        const auto alias = parent.filePath("Alias.societycontainer");
        CTEST_CHECK(createTestSymbolicLink(drive->rootPath(), alias));
        CTEST_CHECK(!SocietyDrive::create(alias)); CTEST_CHECK(!SocietyDrive::open(alias));
        CTEST_CHECK(drive->isValid());
    }
    void createsPackageStorageTreeAndReusesIdentity()
    {
        workspace = std::make_unique<QTemporaryDir>(QDir::current().filePath("drive-parent-XXXXXX"));
        CTEST_CHECK(workspace->isValid());
        QString error;
        const auto drive = SocietyDrive::createAt(workspace->path(), &error);
        CTEST_CHECK_MESSAGE(drive, qPrintable(error));
        CTEST_COMPARE(drive->rootPath(), workspace->filePath("Society.societycontainer"));
        CTEST_CHECK(!QFileInfo::exists(workspace->filePath("Society")));
        CTEST_CHECK(!QFileInfo::exists(drive->rootPath() + "/bands"));
        for (const auto section : allStoreSections())
            CTEST_COMPARE(drive->sectionPath(section), drive->rootPath() + '/' + storeSectionName(section));
        for (const auto type : allModelTypes())
            CTEST_CHECK(QFileInfo(drive->sectionPath(StoreSection::Models) + '/' + modelTypeName(type)).isDir());
        CTEST_COMPARE(SocietyDrive::createAt(workspace->path())->identifier(), drive->identifier());
        CTEST_COMPARE(SocietyDrive::createAt(drive->rootPath())->identifier(), drive->identifier());
        CTEST_COMPARE(QDir(drive->sectionPath(StoreSection::Models)).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), allModelTypes().size());
        CTEST_CHECK(!SocietyDrive::createAt(drive->sectionPath(StoreSection::Files), &error));
        CTEST_CHECK(!QFileInfo::exists(drive->sectionPath(StoreSection::Files) + "/Society.societycontainer"));
    }
    void refusesConflictingOrRedirectedStorageFolder()
    {
        workspace = std::make_unique<QTemporaryDir>(QDir::current().filePath("drive-parent-XXXXXX"));
        CTEST_CHECK(workspace->isValid());
        QFile conflict(workspace->filePath("Society.societycontainer"));
        CTEST_CHECK(conflict.open(QIODevice::WriteOnly)); conflict.write("keep"); conflict.close();
        CTEST_CHECK(!SocietyDrive::createAt(workspace->path()));
        CTEST_CHECK(conflict.open(QIODevice::ReadOnly)); CTEST_COMPARE(conflict.readAll(), QByteArray("keep")); conflict.close();
        CTEST_CHECK(conflict.remove());
        CTEST_CHECK(QDir().mkdir(workspace->filePath("outside")));
        CTEST_CHECK(QFile::link(workspace->filePath("outside"), workspace->filePath("Society.societycontainer")));
        CTEST_CHECK(!SocietyDrive::createAt(workspace->path()));
        CTEST_CHECK(!QFileInfo::exists(workspace->filePath("outside/.society-drive.json")));
    }
    void accessesLogicalSectionTreesAndRejectsReplacedIdentity()
    {
        const auto drive = SocietyDrive::create(workspace->path()); CTEST_CHECK(drive);
        CTEST_CHECK(QDir().mkpath(workspace->filePath("Files/projects/empty")));
        QFile file(workspace->filePath("Files/projects/note.txt"));
        CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("tree bytes"); file.close();
        auto snapshot = drive->tree(); CTEST_CHECK(snapshot);
        CTEST_COMPARE(snapshot.value->children.size(), std::size_t(9));
        const auto* note = snapshot.value->find("Files/projects/note.txt"); CTEST_CHECK(note);
        CTEST_COMPARE(note->size, std::uintmax_t(10));
        CTEST_CHECK(note->parentPath == "Files/projects");
        CTEST_CHECK(note->path == std::filesystem::path(file.fileName().toStdString()));
        CTEST_CHECK(!snapshot.value->find(".society-drive.json"));
        const auto subtree = drive->tree("Files/projects"); CTEST_CHECK(subtree);
        CTEST_CHECK(subtree.value->relativePath == "Files/projects");
        CTEST_CHECK(subtree.value->find("Files/projects/empty")->childrenLoaded);
        auto children = drive->entries("Files/projects"); CTEST_CHECK(children);
        CTEST_COMPARE(children.value->size(), std::size_t(2));
        CTEST_CHECK(drive->entry("Files/projects/note.txt"));
        CTEST_CHECK(!drive->entries("Files/projects/note.txt"));
        CTEST_CHECK(!drive->tree("Files/projects/../../Models"));
        CTEST_CHECK(!drive->tree(".society-sync"));
        CTEST_CHECK(!drive->tree({}, {false, 64, 9}));
        CTEST_CHECK(drive->tree({}, {false, 1, 10}));
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto replica = SocietyDrive::adoptReplicaIdentity(workspace->path(), drive->identifier(), host);
        CTEST_CHECK(replica); CTEST_CHECK(!drive->tree()); CTEST_CHECK(!replica->tree());
        CTEST_CHECK(drive->resolvePath("Files/new.txt").isEmpty());
        CTEST_CHECK(drive->relativePath(file.fileName()).isEmpty());
        CTEST_COMPARE(replica->resolvePath("Files/new/deep.txt"), workspace->filePath("Files/new/deep.txt"));
        CTEST_CHECK(createTestSymbolicLink(workspace->filePath("Models"), workspace->filePath("Files/redirect")));
        CTEST_CHECK(replica->resolvePath("Files/redirect/escape").isEmpty());
        CTEST_CHECK(removeTestSymbolicLink(workspace->filePath("Files/redirect")));
        CTEST_CHECK(replica->resolvePath("Files/projects/note.txt/child").isEmpty());
        CTEST_CHECK(replica->resolvePath("Files/alternate:stream").isEmpty());
        CTEST_CHECK(SocietyDrive::completeReplica(workspace->path(), host));
        CTEST_CHECK(replica->tree());
    }

    void init()
    {
        workspace = std::make_unique<QTemporaryDir>(QDir::current().filePath("society-drive-XXXXXX.societycontainer"));
        CTEST_CHECK(workspace->isValid());
    }

    void createsAndReopensDriveWithNineIndependentRoots()
    {
        QString error;
        const auto drive = SocietyDrive::create(workspace->path(), &error);
        CTEST_CHECK_MESSAGE(drive.has_value(), qPrintable(error));
        CTEST_CHECK(error.isEmpty());
        CTEST_CHECK(drive->isValid());
        CTEST_CHECK(!drive->identifier().isEmpty());
        CTEST_COMPARE(drive->displayName(), QStringLiteral("Society"));
        CTEST_COMPARE(drive->sections(), allStoreSections());
        for (const auto section : drive->sections()) {
            const auto path = drive->sectionPath(section);
            CTEST_CHECK(QFileInfo(path).isDir());
            CTEST_COMPARE(QFileInfo(path).fileName(), storeSectionName(section));
            CTEST_COMPARE(drive->sectionForPath(path).value(), section);
            CTEST_COMPARE(drive->sectionForPath(storeSectionName(section)).value(), section);
        }
        const auto reopened = SocietyDrive::open(workspace->path(), &error);
        CTEST_CHECK_MESSAGE(reopened.has_value(), qPrintable(error));
        CTEST_COMPARE(reopened->identifier(), drive->identifier());
        const auto repeated = SocietyDrive::create(workspace->path(), &error);
        CTEST_CHECK(repeated.has_value());
        CTEST_COMPARE(repeated->identifier(), drive->identifier());
    }

    void migratesLegacyPhotosWithPreviewsAndIdentity()
    {
        const auto drive = SocietyDrive::create(workspace->path()); CTEST_CHECK(drive);
        QFile manifest(workspace->filePath(".society-drive.json"));
        CTEST_CHECK(manifest.open(QIODevice::ReadOnly));
        auto data = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
        QJsonArray legacySections;
        for (const auto &section : data.value("sections").toArray())
            if (section.toObject().value("id") != "photos") legacySections.append(section);
        data["sections"] = legacySections;
        CTEST_CHECK(manifest.open(QIODevice::WriteOnly)); manifest.write(QJsonDocument(data).toJson()); manifest.close();
        QDir().rmdir(workspace->filePath("Photos"));
        CTEST_CHECK(QDir().mkpath(workspace->filePath("Files/Photos/.previews")));
        const QStringList names{"image.societyphoto", ".previews/image.jpg", "clip.mp4"};
        for (const auto &name : names) {
            QFile file(workspace->filePath("Files/Photos/" + name));
            CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write(name.toUtf8());
        }
        QString error;
        const auto upgraded = SocietyDrive::open(workspace->path(), &error); CTEST_CHECK_MESSAGE(upgraded, qPrintable(error));
        CTEST_COMPARE(upgraded->identifier(), drive->identifier());
        CTEST_CHECK(!QFileInfo::exists(workspace->filePath("Files/Photos")));
        for (const auto &name : names) {
            QFile file(workspace->filePath("Photos/" + name));
            CTEST_CHECK(file.open(QIODevice::ReadOnly)); CTEST_COMPARE(file.readAll(), name.toUtf8());
        }
        CTEST_CHECK(manifest.open(QIODevice::ReadOnly));
        const auto migrated = manifest.readAll(); manifest.close();
        CTEST_COMPARE(QJsonDocument::fromJson(migrated).object().value("sections").toArray().size(), 9);
        CTEST_CHECK(SocietyDrive::open(workspace->path()));
        CTEST_CHECK(manifest.open(QIODevice::ReadOnly)); CTEST_COMPARE(manifest.readAll(), migrated);
    }

    void legacyPhotosMergePreservesConflictsAndReplicaState(bool conflict)
    {
        const auto drive = SocietyDrive::create(workspace->path()); CTEST_CHECK(drive);
        QFile manifest(workspace->filePath(".society-drive.json"));
        CTEST_CHECK(manifest.open(QIODevice::ReadOnly));
        auto data = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
        QJsonArray sections;
        for (const auto &entry : data.value("sections").toArray())
            if (entry.toObject().value("id") != "photos") sections.append(entry);
        data["sections"] = sections;
        data["localIdentifier"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        data["replicaReady"] = false;
        const auto oldManifest = QJsonDocument(data).toJson();
        CTEST_CHECK(manifest.open(QIODevice::WriteOnly)); manifest.write(oldManifest); manifest.close();
        CTEST_CHECK(QDir().mkpath(workspace->filePath("Files/Photos/.previews")));
        const auto write = [&](const QString &path, const QByteArray &bytes) {
            QFile file(workspace->filePath(path)); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        const auto read = [&](const QString &path) {
            QFile file(workspace->filePath(path)); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
        };
        CTEST_CHECK(write("Files/Photos/a.societyphoto", "original"));
        CTEST_CHECK(write("Files/Photos/.previews/a.jpg", "preview"));
        CTEST_CHECK(write("Photos/a.societyphoto", conflict ? "different" : "original"));
        CTEST_CHECK(write("Photos/new.jpg", "new"));
        QString error; const auto upgraded = SocietyDrive::open(workspace->path(), &error);
        if (conflict) {
            CTEST_CHECK(!upgraded); CTEST_CHECK(!error.isEmpty());
            CTEST_COMPARE(read("Files/Photos/a.societyphoto"), "original");
            CTEST_COMPARE(read("Files/Photos/.previews/a.jpg"), "preview");
            CTEST_COMPARE(read("Photos/a.societyphoto"), "different");
            CTEST_COMPARE(read(".society-drive.json"), oldManifest);
        } else {
            CTEST_CHECK_MESSAGE(upgraded, qPrintable(error)); CTEST_CHECK(!upgraded->isReady());
            CTEST_COMPARE(upgraded->identifier(), drive->identifier());
            CTEST_CHECK(!QFileInfo::exists(workspace->filePath("Files/Photos")));
            CTEST_COMPARE(read("Photos/a.societyphoto"), "original");
            CTEST_COMPARE(read("Photos/.previews/a.jpg"), "preview");
            const auto upgradedData = QJsonDocument::fromJson(read(".society-drive.json")).object();
            CTEST_COMPARE(upgradedData.value("localIdentifier"), data.value("localIdentifier"));
            CTEST_COMPARE(upgradedData.value("replicaReady"), QJsonValue(false));
        }
        CTEST_COMPARE(read("Photos/new.jpg"), "new");
    }

    void legacyNameKeepsIdentityAndSourceData()
    {
        const auto created = SocietyDrive::create(workspace->path());
        CTEST_CHECK(created);
        QFile manifest(workspace->filePath(".society-drive.json"));
        CTEST_CHECK(manifest.open(QIODevice::ReadOnly));
        auto data = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
        CTEST_COMPARE(data.value("displayName").toString(), QString("Society"));
        data["displayName"] = "Society Container";
        const auto legacy = QJsonDocument(data).toJson();
        CTEST_CHECK(manifest.open(QIODevice::WriteOnly)); CTEST_COMPARE(manifest.write(legacy), legacy.size()); manifest.close();
        QFile file(workspace->filePath("Files/keep.txt")); CTEST_CHECK(file.open(QIODevice::WriteOnly));
        file.write("keep"); file.close();
        const auto reopened = SocietyDrive::open(workspace->path());
        CTEST_CHECK(reopened); CTEST_COMPARE(reopened->identifier(), created->identifier());
        CTEST_COMPARE(reopened->displayName(), QString("Society"));
        CTEST_CHECK(SocietyDrive::create(workspace->path()));
        CTEST_CHECK(manifest.open(QIODevice::ReadOnly)); CTEST_COMPARE(manifest.readAll(), legacy); manifest.close();
        CTEST_CHECK(file.open(QIODevice::ReadOnly)); CTEST_COMPARE(file.readAll(), QByteArray("keep"));
        data["displayName"] = "Unrelated drive";
        CTEST_CHECK(manifest.open(QIODevice::WriteOnly)); manifest.write(QJsonDocument(data).toJson()); manifest.close();
        CTEST_CHECK(!SocietyDrive::open(workspace->path()));
    }

    void adoptsHostIdentityWithoutMovingSectionsOrLosingData()
    {
        const auto original = SocietyDrive::create(workspace->path()); CTEST_CHECK(original);
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QFile file(workspace->filePath("Files/keep.txt")); CTEST_CHECK(file.open(QIODevice::WriteOnly));
        file.write("keep"); file.close();
        QString error;
        CTEST_CHECK(!SocietyDrive::adoptReplicaIdentity(workspace->path(), original->identifier(), "invalid", &error));
        CTEST_CHECK(!error.isEmpty());
        CTEST_CHECK(!SocietyDrive::adoptReplicaIdentity(workspace->path(), "wrong", host, &error));
        CTEST_CHECK(original->isValid());
        const auto mirror = SocietyDrive::adoptReplicaIdentity(workspace->path(), original->identifier(), host, &error);
        CTEST_CHECK_MESSAGE(mirror, qPrintable(error)); CTEST_COMPARE(mirror->identifier(), host);
        CTEST_COMPARE(mirror->rootPath(), original->rootPath());
        CTEST_CHECK(!original->isValid()); CTEST_CHECK(mirror->isValid());
        CTEST_COMPARE(SocietyDrive::create(workspace->path())->identifier(), host);
        CTEST_CHECK(SocietyDrive::adoptReplicaIdentity(workspace->path(), original->identifier(), host));
        CTEST_CHECK(file.open(QIODevice::ReadOnly)); CTEST_COMPARE(file.readAll(), QByteArray("keep"));
        CTEST_COMPARE(mirror->sections(), allStoreSections());
        CTEST_CHECK(!SocietyDrive::completeReplica(workspace->path(), original->identifier(), &error));
        CTEST_CHECK(SocietyDrive::completeReplica(workspace->path(), host, &error));
        QFile manifest(workspace->filePath(".society-drive.json")); CTEST_CHECK(manifest.open(QIODevice::ReadOnly));
        const auto data = QJsonDocument::fromJson(manifest.readAll()).object();
        CTEST_COMPARE(data.value("localIdentifier").toString(), original->identifier());
        CTEST_CHECK(data.value("replicaReady").toBool());
        manifest.close();
        const auto repeatedAdoption = SocietyDrive::adoptReplicaIdentity(workspace->path(), host, host);
        CTEST_CHECK(repeatedAdoption); CTEST_CHECK(!repeatedAdoption->isReady());
        CTEST_CHECK(SocietyDrive::completeReplica(workspace->path(), host));
    }

    void keepsExistingContentAndRejectsDirectoryConflictsBeforeCreation()
    {
        QFile conflict(workspace->filePath("Models"));
        CTEST_CHECK(conflict.open(QIODevice::WriteOnly));
        CTEST_COMPARE(conflict.write("original"), qint64(8));
        conflict.close();
        QString error;
        CTEST_CHECK(!SocietyDrive::create(workspace->path(), &error));
        CTEST_CHECK(!error.isEmpty());
        CTEST_CHECK(!QFileInfo::exists(workspace->filePath("Asset Library")));
        CTEST_CHECK(!QFileInfo::exists(workspace->filePath(".society-drive.json")));
        CTEST_CHECK(conflict.open(QIODevice::ReadOnly));
        CTEST_COMPARE(conflict.readAll(), QByteArray("original"));
    }

    void preservesUnassignedFiles()
    {
        QFile file(workspace->filePath("existing.txt"));
        CTEST_CHECK(file.open(QIODevice::WriteOnly));
        CTEST_COMPARE(file.write("original"), qint64(8));
        file.close();
        const auto drive = SocietyDrive::create(workspace->path());
        CTEST_CHECK(drive.has_value());
        CTEST_CHECK(!drive->sectionForPath(file.fileName()));
        CTEST_CHECK(!drive->sectionForPath("."));
        CTEST_CHECK(!drive->sectionForPath("../"));
        CTEST_CHECK(file.open(QIODevice::ReadOnly));
        CTEST_COMPARE(file.readAll(), QByteArray("original"));
    }

    void rejectsNestedContainersBeforeWriting()
    {
        const auto drive = SocietyDrive::create(workspace->path());
        CTEST_CHECK(drive);
        const auto files = drive->sectionPath(StoreSection::Files);
        QString error;
        CTEST_CHECK(!SocietyDrive::create(files, &error));
        CTEST_CHECK(error.contains("source"));
        CTEST_COMPARE(QDir(files).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        CTEST_CHECK(!QFileInfo::exists(QDir(files).filePath(".society-drive.json")));
        CTEST_CHECK(QDir().mkpath(QDir(files).filePath("Nested/Deep")));
        CTEST_CHECK(!SocietyDrive::create(QDir(files).filePath("Nested/Deep"), &error));
        CTEST_CHECK(QDir(QDir(files).filePath("Nested/Deep")).isEmpty());

        // Reproduce a container written by an older app inside the public area.
        for (const auto section : allStoreSections())
            CTEST_CHECK(QDir().mkdir(QDir(files).filePath(storeSectionName(section))));
        CTEST_CHECK(QFile::copy(workspace->filePath(".society-drive.json"), QDir(files).filePath(".society-drive.json")));
        CTEST_CHECK(!SocietyDrive::open(files, &error));
        CTEST_CHECK(!SocietyDrive::create(files, &error));
        CTEST_CHECK(QFileInfo::exists(QDir(files).filePath(".society-drive.json")));
#ifdef Q_OS_UNIX
        const auto alias = workspace->filePath("alias");
        CTEST_CHECK(createTestSymbolicLink(files, alias));
        CTEST_CHECK(!SocietyDrive::open(alias, &error));
#endif
        CTEST_CHECK(drive->isValid());
    }

    void rejectsFinderReplicasAndTheirDescendants()
    {
        workspace = std::make_unique<QTemporaryDir>(QDir::current().filePath("drive-parent-XXXXXX"));
        CTEST_CHECK(workspace->isValid());
#ifdef Q_OS_MACOS
        const auto cloud = workspace->filePath("Library/CloudStorage");
        const auto replica = QDir(cloud).filePath("SocietyContainer-SocietyContainer.societycontainer");
        CTEST_CHECK(QDir().mkpath(replica));
        // An existing misplaced manifest must be rejected as well as new creation.
        CTEST_CHECK(SocietyDrive::create(replica));
        const auto homeBefore = qgetenv("HOME");
        const auto restoreHome = qScopeGuard([homeBefore] { qputenv("HOME", homeBefore); });
        qputenv("HOME", workspace->path().toUtf8());
        CTEST_COMPARE(QDir::homePath(), workspace->path());
        QString error;
        CTEST_CHECK(!SocietyDrive::open(replica, &error));
        CTEST_CHECK(error.contains("Finder"));
        CTEST_CHECK(!SocietyDrive::create(replica, &error));
        const auto child = QDir(replica).filePath("New Folder");
        CTEST_CHECK(QDir().mkdir(child));
        CTEST_CHECK(!SocietyDrive::create(child, &error));
        CTEST_CHECK(QDir(child).isEmpty());
        const auto alias = workspace->filePath("replica-alias");
        CTEST_CHECK(createTestSymbolicLink(replica, alias));
        CTEST_CHECK(!SocietyDrive::open(alias, &error));
        const auto sibling = workspace->filePath("Library/CloudStorage-backup.societycontainer");
        CTEST_CHECK(QDir().mkdir(sibling));
        CTEST_CHECK(SocietyDrive::create(sibling, &error));
#else
        society_ctest::skip("Finder replicas are a macOS source-location boundary.");
#endif
    }

    void doesNotInferAnUninitializedDrive()
    {
        QString error;
        CTEST_CHECK(!SocietyDrive::open(workspace->path(), &error));
        CTEST_CHECK(!error.isEmpty());
        CTEST_CHECK(!SocietyDrive::create(QString(), &error));
        CTEST_CHECK(!SocietyDrive::create(workspace->filePath("missing"), &error));
        CTEST_CHECK(!QFileInfo::exists(workspace->filePath("missing")));
    }

    void rejectsInvalidManifestWithoutOverwritingIt()
    {
        QFile manifest(workspace->filePath(".society-drive.json"));
        CTEST_CHECK(manifest.open(QIODevice::WriteOnly));
        const QByteArray data("{\"type\":\"another-format\"}");
        CTEST_COMPARE(manifest.write(data), data.size());
        manifest.close();
        QString error;
        CTEST_CHECK(!SocietyDrive::create(workspace->path(), &error));
        CTEST_CHECK(!SocietyDrive::open(workspace->path(), &error));
        CTEST_CHECK(!error.isEmpty());
        CTEST_CHECK(manifest.open(QIODevice::ReadOnly));
        CTEST_COMPARE(manifest.readAll(), data);
    }

    void detectsMissingSectionsAndInvalidSectionValues()
    {
        const auto drive = SocietyDrive::create(workspace->path());
        CTEST_CHECK(drive.has_value());
        CTEST_CHECK(drive->sectionPath(static_cast<StoreSection>(-1)).isEmpty());
        CTEST_CHECK(QDir().rename(drive->sectionPath(StoreSection::Models), workspace->filePath("Models-original")));
        CTEST_CHECK(!drive->isValid());
        CTEST_CHECK(!SocietyDrive::open(workspace->path()));
    }

    void preventsSectionsFromAliasingEachOther()
    {
#ifdef Q_OS_UNIX
        CTEST_CHECK(QDir().mkdir(workspace->filePath("Files")));
        CTEST_CHECK(createTestSymbolicLink(workspace->filePath("Files"), workspace->filePath("Models")));
        QString error;
        CTEST_CHECK(!SocietyDrive::create(workspace->path(), &error));
        CTEST_CHECK(!error.isEmpty());
        CTEST_CHECK(!QFileInfo::exists(workspace->filePath("Asset Library")));
#else
        society_ctest::skip("QFile::link creates shortcuts on Windows.");
#endif
    }

private:
    std::unique_ptr<QTemporaryDir> workspace;
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    society_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    auto runCase = [&](std::string_view name, auto function) {
        runner.run(name, [&] {
            const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); });
            SocietyDriveTests tests;
            tests.init();
            function(tests);
        });
    };
    runCase("replicaIdentityUpdatesWhileMaintenanceReadsTheManifest", [](auto& tests) { tests.replicaIdentityUpdatesWhileMaintenanceReadsTheManifest(); });
    runCase("nativeAndQtUnicodeParentsPreserveThePackageBoundary", [](auto& tests) { tests.nativeAndQtUnicodeParentsPreserveThePackageBoundary(); });
    runCase("bareFoldersImagesAndRedirectedPackagesAreRejectedWithoutWrites", [](auto& tests) { tests.bareFoldersImagesAndRedirectedPackagesAreRejectedWithoutWrites(); });
    runCase("createsPackageStorageTreeAndReusesIdentity", [](auto& tests) { tests.createsPackageStorageTreeAndReusesIdentity(); });
    runCase("refusesConflictingOrRedirectedStorageFolder", [](auto& tests) { tests.refusesConflictingOrRedirectedStorageFolder(); });
    runCase("accessesLogicalSectionTreesAndRejectsReplacedIdentity", [](auto& tests) { tests.accessesLogicalSectionTreesAndRejectsReplacedIdentity(); });
    runCase("createsAndReopensDriveWithNineIndependentRoots", [](auto& tests) { tests.createsAndReopensDriveWithNineIndependentRoots(); });
    runCase("migratesLegacyPhotosWithPreviewsAndIdentity", [](auto& tests) { tests.migratesLegacyPhotosWithPreviewsAndIdentity(); });
    runCase("legacyPhotosMergePreservesConflictsAndReplicaState:identical-duplicate", [](auto& tests) { tests.legacyPhotosMergePreservesConflictsAndReplicaState(false); });
    runCase("legacyPhotosMergePreservesConflictsAndReplicaState:conflicting-content", [](auto& tests) { tests.legacyPhotosMergePreservesConflictsAndReplicaState(true); });
    runCase("legacyNameKeepsIdentityAndSourceData", [](auto& tests) { tests.legacyNameKeepsIdentityAndSourceData(); });
    runCase("adoptsHostIdentityWithoutMovingSectionsOrLosingData", [](auto& tests) { tests.adoptsHostIdentityWithoutMovingSectionsOrLosingData(); });
    runCase("keepsExistingContentAndRejectsDirectoryConflictsBeforeCreation", [](auto& tests) { tests.keepsExistingContentAndRejectsDirectoryConflictsBeforeCreation(); });
    runCase("preservesUnassignedFiles", [](auto& tests) { tests.preservesUnassignedFiles(); });
    runCase("rejectsNestedContainersBeforeWriting", [](auto& tests) { tests.rejectsNestedContainersBeforeWriting(); });
    runCase("rejectsFinderReplicasAndTheirDescendants", [](auto& tests) { tests.rejectsFinderReplicasAndTheirDescendants(); });
    runCase("doesNotInferAnUninitializedDrive", [](auto& tests) { tests.doesNotInferAnUninitializedDrive(); });
    runCase("rejectsInvalidManifestWithoutOverwritingIt", [](auto& tests) { tests.rejectsInvalidManifestWithoutOverwritingIt(); });
    runCase("detectsMissingSectionsAndInvalidSectionValues", [](auto& tests) { tests.detectsMissingSectionsAndInvalidSectionValues(); });
    runCase("preventsSectionsFromAliasingEachOther", [](auto& tests) { tests.preventsSectionsFromAliasingEachOther(); });
    return runner.result();
}
