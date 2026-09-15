#include <FilesView.h>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace iiSocietyContainer;

class FilesViewTests : public QObject {
    Q_OBJECT
private slots:
    void fixedDirectoriesExistAndRestoreOnOpen()
    {
        QTemporaryDir temporary(SOCIETY_TEST_DIRECTORY "/fixed-files-XXXXXX");
        const auto drive = SocietyDrive::create(temporary.path());
        QVERIFY(drive);
        const QStringList names{"Documents", "Audios", "3D objects"};
        for (const auto &name : names)
            QVERIFY2(QFileInfo(temporary.filePath("Files/" + name)).isDir(), qPrintable(name));
        const auto identifier = drive->identifier();
        QVERIFY(QDir().rmdir(temporary.filePath("Files/Documents")));
        const auto reopened = SocietyDrive::open(temporary.path());
        QVERIFY(reopened);
        QCOMPARE(reopened->identifier(), identifier);
        QVERIFY(QFileInfo(temporary.filePath("Files/Documents")).isDir());
    }

    void directoryObjectsPreserveManualOrganizationAndIdentity()
    {
        QTemporaryDir temporary(SOCIETY_TEST_DIRECTORY "/files-objects-XXXXXX");
        QVERIFY(SocietyDrive::create(temporary.path()));
        auto view = FilesView::open(temporary.path()); QVERIFY(view);
        QVERIFY(!view->directory(FileDirectoryKind::Photos));
        QVERIFY(!FilesView::isProtectedPath("Photos"));
        const auto directories = view->directories(); QCOMPARE(directories.size(), 3);
        QStringList names, keys;
        for (const auto &directory : directories) {
            names.append(directory.name()); keys.append(directory.key());
            QVERIFY(directory.isValid()); QVERIFY(directory.isProtected());
            QCOMPARE(directory.path(), temporary.filePath("Files/" + directory.name()));
            QVERIFY(FilesView::isProtectedPath(directory.name()));
            QVERIFY(FilesView::isProtectedPath(directory.name().toLower()));
            QVERIFY(!FilesView::isProtectedPath(directory.name() + "/child"));
            QVERIFY(!FilesView::isProtectedPath("Custom/" + directory.name()));
        }
        QCOMPARE(names, (QStringList{"Documents", "Audios", "3D objects"}));
        QCOMPARE(keys, (QStringList{"documents", "audios", "objects3d"}));
        const auto photos = view->directory(FileDirectoryKind::Documents); QVERIFY(photos);
        for (const auto &name : {"photo.jpg", "video.mp4", "notes.txt"}) {
            QFile file(QDir(photos->path()).filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("manual"), 6);
        }
        QFile loose(temporary.filePath("Files/loose.mp4"));
        QVERIFY(loose.open(QIODevice::WriteOnly)); loose.write("stay here"); loose.close();
        QVERIFY(SocietyDrive::open(temporary.path()));
        QCOMPARE(view->entries("Documents").size(), 3);
        QVERIFY(loose.exists()); QVERIFY(!QFileInfo::exists(temporary.filePath("Files/Documents/loose.mp4")));
        QVERIFY(FilesView::isProtectedPath({}));
        QVERIFY(!FilesView::isProtectedPath("Photos backup"));
        QVERIFY(!view->directory(static_cast<FileDirectoryKind>(-1)));
        QVERIFY(QFile::remove(temporary.filePath(".society-drive.json")));
        QVERIFY(SocietyDrive::create(temporary.path()));
        QVERIFY(!photos->isValid()); QVERIFY(photos->path().isEmpty());
        QVERIFY(view->directories().isEmpty());
    }

    void incompleteReplicasValidateDirectoriesBeforePublication()
    {
        QTemporaryDir temporary(SOCIETY_TEST_DIRECTORY "/files-incomplete-XXXXXX");
        const auto initial = SocietyDrive::create(temporary.path()); QVERIFY(initial);
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(SocietyDrive::adoptReplicaIdentity(temporary.path(), initial->identifier(), host));
        QVERIFY(QDir().rmdir(temporary.filePath("Files/Documents")));
        auto view = FilesView::open(temporary.path()); QVERIFY(view);
        QVERIFY(view->directories().isEmpty()); QVERIFY(!view->directory(FileDirectoryKind::Documents));
        QVERIFY(!QFileInfo::exists(temporary.filePath("Files/Documents")));
        QFile conflict(temporary.filePath("Files/Documents"));
        QVERIFY(conflict.open(QIODevice::WriteOnly)); conflict.write("preserve"); conflict.close();
        QVERIFY(!SocietyDrive::completeReplica(temporary.path(), host));
        QVERIFY(!SocietyDrive::open(temporary.path())->isReady());
        QVERIFY(conflict.rename(temporary.filePath("Files/old-photos")));
        QVERIFY(SocietyDrive::completeReplica(temporary.path(), host));
        QVERIFY(view->directory(FileDirectoryKind::Documents)->isValid());
        QVERIFY(QFileInfo(temporary.filePath("Files/Documents")).isDir());
    }

    void legacyUpgradePreservesContentsAndRejectsConflictingEntries()
    {
        QTemporaryDir temporary(SOCIETY_TEST_DIRECTORY "/files-upgrade-XXXXXX");
        const auto drive = SocietyDrive::create(temporary.path()); QVERIFY(drive);
        for (const auto kind : allFileDirectoryKinds())
            QVERIFY(QDir().rmdir(temporary.filePath("Files/" + fileDirectoryName(kind))));
        QFile conflict(temporary.filePath("Files/Documents"));
        QVERIFY(conflict.open(QIODevice::WriteOnly)); conflict.write("existing data"); conflict.close();
        QString error;
        QVERIFY(!SocietyDrive::open(temporary.path(), &error)); QVERIFY(error.contains("conflicts"));
        QVERIFY(!QFileInfo::exists(temporary.filePath("Files/Audios")));
        QVERIFY(conflict.open(QIODevice::ReadOnly)); QCOMPARE(conflict.readAll(), "existing data"); conflict.close();
        QVERIFY(conflict.rename(temporary.filePath("Files/old-photo-data")));
#ifndef Q_OS_WIN
        QVERIFY(QFile::link(temporary.filePath("Models"), temporary.filePath("Files/Documents")));
        QVERIFY(!SocietyDrive::open(temporary.path(), &error));
        QVERIFY(!QFileInfo::exists(temporary.filePath("Files/Audios")));
        QVERIFY(QFile::remove(temporary.filePath("Files/Documents")));
#endif
        QVERIFY(QDir().mkdir(temporary.filePath("Files/Documents")));
        QFile video(temporary.filePath("Files/Documents/keep.mp4"));
        QVERIFY(video.open(QIODevice::WriteOnly)); video.write("video"); video.close();
        const auto upgraded = SocietyDrive::open(temporary.path(), &error); QVERIFY2(upgraded, qPrintable(error));
        QCOMPARE(upgraded->identifier(), drive->identifier());
        QVERIFY(video.open(QIODevice::ReadOnly)); QCOMPARE(video.readAll(), "video");
        QCOMPARE(FilesView::open(temporary.path())->directories().size(), 3);
    }

    void projectsFilesAndPreservesPrivateSections()
    {
        QTemporaryDir temporary(SOCIETY_TEST_DIRECTORY "/files-view-XXXXXX");
        auto drive = SocietyDrive::create(temporary.path());
        QVERIFY(drive);
        QFile privateFile(temporary.filePath("Models/private.safetensor"));
        QVERIFY(privateFile.open(QIODevice::WriteOnly));
        privateFile.write("private model"); privateFile.close();
        QFile publicFile(temporary.filePath("Files/document.txt"));
        QVERIFY(publicFile.open(QIODevice::WriteOnly));
        publicFile.write("public document"); publicFile.close();
        auto view = FilesView::open(temporary.path());
        QVERIFY(view);
        QCOMPARE(view->rootPath(), drive->sectionPath(StoreSection::Files));
        QCOMPARE(view->resolve({}), view->rootPath());
        const auto children = view->entries();
        QCOMPARE(children.size(), 4);
        QVERIFY(std::any_of(children.begin(), children.end(), [](const auto &entry) { return entry.fileName() == "document.txt"; }));
        for (const auto section : allStoreSections())
            QVERIFY(view->resolve(storeSectionName(section)).isEmpty());
        QVERIFY(view->resolve(".society-drive.json").isEmpty());
        QCOMPARE(view->resolve("document.txt"), publicFile.fileName());
        QFile throughView(view->resolve("created.txt", true));
        QVERIFY(throughView.open(QIODevice::WriteOnly));
        throughView.write("created through the drive"); throughView.close();
        QVERIFY(QFileInfo::exists(temporary.filePath("Files/created.txt")));
        QVERIFY(!QFileInfo::exists(temporary.filePath("created.txt")));
        QVERIFY(privateFile.open(QIODevice::ReadOnly));
        QCOMPARE(privateFile.readAll(), "private model");
    }

    void rejectsPaths_data()
    {
        QTest::addColumn<QString>("path");
        for (const auto value : {"../Models/private", "folder/../../Models", "/Models", "C:/Models",
                                 "folder\\name", "a//b", "a/./b", "a/../b", "a/", ".", ".."})
            QTest::newRow(value) << QString::fromUtf8(value);
        QTest::newRow("nul") << QString("bad") + QChar::Null + "name";
    }
    void rejectsPaths()
    {
        QFETCH(QString, path);
        QTemporaryDir temporary(SOCIETY_TEST_DIRECTORY "/files-view-XXXXXX");
        QVERIFY(SocietyDrive::create(temporary.path()));
        auto view = FilesView::open(temporary.path());
        QVERIFY(view);
        QString error;
        QVERIFY(view->resolve(path, true, &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
    void nestedNamesAndHiddenFiles()
    {
        QTemporaryDir temporary(SOCIETY_TEST_DIRECTORY "/files-view-XXXXXX");
        QVERIFY(SocietyDrive::create(temporary.path()));
        QVERIFY(QDir().mkpath(temporary.filePath("Files/한글 folder/.hidden")));
        auto view = FilesView::open(temporary.path());
        QVERIFY(view);
        QCOMPARE(view->resolve("한글 folder/.hidden/new.txt", true), temporary.filePath("Files/한글 folder/.hidden/new.txt"));
        QVERIFY(view->resolve("missing/new.txt", true).isEmpty());
        QCOMPARE(view->entries("한글 folder").size(), 1);
    }
    void rejectsRedirectionAndDriveReplacement()
    {
        QTemporaryDir temporary(SOCIETY_TEST_DIRECTORY "/files-view-XXXXXX");
        QVERIFY(SocietyDrive::create(temporary.path()));
        auto view = FilesView::open(temporary.path());
        QVERIFY(view);
#ifndef Q_OS_WIN
        QVERIFY(QFile::link(temporary.filePath("Models"), temporary.filePath("Files/private-link")));
        QVERIFY(view->resolve("private-link", true).isEmpty());
        QVERIFY(view->resolve("private-link/model", true).isEmpty());
        QCOMPARE(view->entries().size(), 3);
#endif
        QVERIFY(QFile::remove(temporary.filePath(".society-drive.json")));
        QVERIFY(SocietyDrive::create(temporary.path()));
        QVERIFY(!view->isValid());
        QVERIFY(view->resolve("new", true).isEmpty());
    }
};
QTEST_GUILESS_MAIN(FilesViewTests)
#include "files_view.moc"
