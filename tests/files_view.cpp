#include <FilesView.h>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace iiSocietyContainer;

class FilesViewTests : public QObject {
    Q_OBJECT
private slots:
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
        QCOMPARE(children.first().fileName(), "document.txt");
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
        QVERIFY(view->entries().isEmpty());
#endif
        QVERIFY(QFile::remove(temporary.filePath(".society-drive.json")));
        QVERIFY(SocietyDrive::create(temporary.path()));
        QVERIFY(!view->isValid());
        QVERIFY(view->resolve("new", true).isEmpty());
    }
};
QTEST_GUILESS_MAIN(FilesViewTests)
#include "files_view.moc"
