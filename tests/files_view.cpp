#include <FilesView.h>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace iiSocietyContainer;

class FilesViewTests : public QObject {
    Q_OBJECT
private slots:
    void filesStartEmptyAndStayEmptyAcrossReopenAndReplicaCompletion()
    {
        QTemporaryDir temporary(SOCIETY_TEST_DIRECTORY "/empty-files-XXXXXX");
        const auto drive = SocietyDrive::create(temporary.path()); QVERIFY(drive);
        auto view = FilesView::open(temporary.path()); QVERIFY(view);
        QVERIFY(view->entries().isEmpty()); QVERIFY(view->directories().isEmpty());
        QVERIFY(allFileDirectoryKinds().isEmpty());
        for (const auto kind : {FileDirectoryKind::Documents, FileDirectoryKind::Photos,
                                FileDirectoryKind::Audios, FileDirectoryKind::Objects3D})
            QVERIFY(!view->directory(kind));
        QVERIFY(SocietyDrive::open(temporary.path()));
        QVERIFY(view->entries().isEmpty());
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(SocietyDrive::adoptReplicaIdentity(temporary.path(), drive->identifier(), host));
        QVERIFY(SocietyDrive::open(temporary.path()));
        QVERIFY(SocietyDrive::completeReplica(temporary.path(), host));
        QVERIFY(FilesView::open(temporary.path())->entries().isEmpty());
    }

    void formerDefaultNamesAreOrdinaryUserEntries()
    {
        QTemporaryDir temporary(SOCIETY_TEST_DIRECTORY "/user-files-XXXXXX");
        QVERIFY(SocietyDrive::create(temporary.path()));
        for (const auto &name : {"Documents", "Audios", "3D objects"}) {
            QVERIFY(!FilesView::isProtectedPath(name));
            QVERIFY(!isFixedFilesDirectory(QString(name).toLower()));
            QVERIFY(QDir().mkdir(temporary.filePath("Files/" + QString(name))));
        }
        QVERIFY(SocietyDrive::open(temporary.path()));
        QCOMPARE(FilesView::open(temporary.path())->entries().size(), 3);
        QVERIFY(QDir().rmdir(temporary.filePath("Files/Documents")));
        QFile namedFile(temporary.filePath("Files/Documents"));
        QVERIFY(namedFile.open(QIODevice::WriteOnly)); namedFile.write("user file"); namedFile.close();
        QVERIFY(SocietyDrive::open(temporary.path()));
        QVERIFY(namedFile.open(QIODevice::ReadOnly)); QCOMPARE(namedFile.readAll(), "user file");
        QVERIFY(FilesView::isProtectedPath({}));
    }

    void legacyUpgradeRemovesOnlyEmptyDefaultsOnce()
    {
        QTemporaryDir temporary(SOCIETY_TEST_DIRECTORY "/legacy-files-XXXXXX");
        const auto drive = SocietyDrive::create(temporary.path()); QVERIFY(drive);
        for (const auto &name : {"Documents", "Audios", "3D objects", "Custom"})
            QVERIFY(QDir().mkdir(temporary.filePath("Files/" + QString(name))));
        QFile keep(temporary.filePath("Files/Documents/.keep"));
        QVERIFY(keep.open(QIODevice::WriteOnly)); keep.write("preserve hidden data"); keep.close();
        QFile manifest(temporary.filePath(".society-drive.json"));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        auto data = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
        data.remove("filesLayoutVersion");
        QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        manifest.write(QJsonDocument(data).toJson()); manifest.close();
        const auto reopened = SocietyDrive::open(temporary.path()); QVERIFY(reopened);
        QCOMPARE(reopened->identifier(), drive->identifier());
        QVERIFY(keep.open(QIODevice::ReadOnly)); QCOMPARE(keep.readAll(), "preserve hidden data"); keep.close();
        QVERIFY(QFileInfo(temporary.filePath("Files/Custom")).isDir());
        QVERIFY(!QFileInfo::exists(temporary.filePath("Files/Audios")));
        QVERIFY(!QFileInfo::exists(temporary.filePath("Files/3D objects")));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(manifest.readAll()).object().value("filesLayoutVersion").toInt(), 1);
        manifest.close();
        QVERIFY(QDir().mkdir(temporary.filePath("Files/Audios")));
        QVERIFY(SocietyDrive::open(temporary.path()));
        QVERIFY(QFileInfo(temporary.filePath("Files/Audios")).isDir());
    }

    void legacyReplicasDeferCleanupUntilCompletion()
    {
        QTemporaryDir temporary(SOCIETY_TEST_DIRECTORY "/legacy-files-replica-XXXXXX");
        const auto initial = SocietyDrive::create(temporary.path()); QVERIFY(initial);
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(SocietyDrive::adoptReplicaIdentity(temporary.path(), initial->identifier(), host));
        QVERIFY(QDir().mkdir(temporary.filePath("Files/Audios")));
        QFile manifest(temporary.filePath(".society-drive.json"));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        auto data = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
        data.remove("filesLayoutVersion");
        QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        manifest.write(QJsonDocument(data).toJson()); manifest.close();
        QVERIFY(!SocietyDrive::open(temporary.path())->isReady());
        QVERIFY(QFileInfo(temporary.filePath("Files/Audios")).isDir());
        QVERIFY(SocietyDrive::completeReplica(temporary.path(), host));
        QVERIFY(FilesView::open(temporary.path())->entries().isEmpty());
        QVERIFY(QDir().mkdir(temporary.filePath("Files/Audios")));
        QVERIFY(SocietyDrive::open(temporary.path()));
        QVERIFY(QFileInfo(temporary.filePath("Files/Audios")).isDir());
    }

    void legacyUpgradePreservesFilesAndRedirectedEntries()
    {
        QTemporaryDir temporary(SOCIETY_TEST_DIRECTORY "/legacy-file-names-XXXXXX");
        QVERIFY(SocietyDrive::create(temporary.path()));
        QFile file(temporary.filePath("Files/Documents"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("ordinary file"); file.close();
#ifndef Q_OS_WIN
        QVERIFY(QFile::link(temporary.filePath("Models"), temporary.filePath("Files/Audios")));
#endif
        QFile manifest(temporary.filePath(".society-drive.json"));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        auto data = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
        data.remove("filesLayoutVersion");
        QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        manifest.write(QJsonDocument(data).toJson()); manifest.close();
        QVERIFY(SocietyDrive::open(temporary.path()));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), "ordinary file");
#ifndef Q_OS_WIN
        QVERIFY(QFileInfo(temporary.filePath("Files/Audios")).isSymLink());
        QVERIFY(FilesView::open(temporary.path())->resolve("Audios").isEmpty());
#endif
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
        QCOMPARE(children.size(), 1);
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
        QCOMPARE(view->entries().size(), 0);
#endif
        QVERIFY(QFile::remove(temporary.filePath(".society-drive.json")));
        QVERIFY(SocietyDrive::create(temporary.path()));
        QVERIFY(!view->isValid());
        QVERIFY(view->resolve("new", true).isEmpty());
    }
};
QTEST_GUILESS_MAIN(FilesViewTests)
#include "files_view.moc"
