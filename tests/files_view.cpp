#include "SymbolicLink.h"
#include <FilesView.h>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>
#include "CTestSupport.h"
#include <QCoreApplication>

using namespace iiSocietyContainer;

class FilesViewTests : public QObject {
public:
    void filesStartEmptyAndStayEmptyAcrossReopenAndReplicaCompletion()
    {
        QTemporaryDir temporary(QDir::current().filePath("empty-files-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(temporary.path()); CTEST_CHECK(drive);
        auto view = FilesView::open(temporary.path()); CTEST_CHECK(view);
        CTEST_CHECK(view->entries().isEmpty()); CTEST_CHECK(view->directories().isEmpty());
        CTEST_CHECK(allFileDirectoryKinds().isEmpty());
        for (const auto kind : {FileDirectoryKind::Documents, FileDirectoryKind::Photos,
                                FileDirectoryKind::Audios, FileDirectoryKind::Objects3D})
            CTEST_CHECK(!view->directory(kind));
        CTEST_CHECK(SocietyDrive::open(temporary.path()));
        CTEST_CHECK(view->entries().isEmpty());
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        CTEST_CHECK(SocietyDrive::adoptReplicaIdentity(temporary.path(), drive->identifier(), host));
        CTEST_CHECK(SocietyDrive::open(temporary.path()));
        CTEST_CHECK(SocietyDrive::completeReplica(temporary.path(), host));
        CTEST_CHECK(FilesView::open(temporary.path())->entries().isEmpty());
    }

    void formerDefaultNamesAreOrdinaryUserEntries()
    {
        QTemporaryDir temporary(QDir::current().filePath("user-files-XXXXXX.societycontainer"));
        CTEST_CHECK(SocietyDrive::create(temporary.path()));
        for (const auto &name : {"Documents", "Audios", "3D objects"}) {
            CTEST_CHECK(!FilesView::isProtectedPath(name));
            CTEST_CHECK(!isFixedFilesDirectory(QString(name).toLower()));
            CTEST_CHECK(QDir().mkdir(temporary.filePath("Files/" + QString(name))));
        }
        CTEST_CHECK(SocietyDrive::open(temporary.path()));
        CTEST_COMPARE(FilesView::open(temporary.path())->entries().size(), 3);
        CTEST_CHECK(QDir().rmdir(temporary.filePath("Files/Documents")));
        QFile namedFile(temporary.filePath("Files/Documents"));
        CTEST_CHECK(namedFile.open(QIODevice::WriteOnly)); namedFile.write("user file"); namedFile.close();
        CTEST_CHECK(SocietyDrive::open(temporary.path()));
        CTEST_CHECK(namedFile.open(QIODevice::ReadOnly)); CTEST_COMPARE(namedFile.readAll(), "user file");
        CTEST_CHECK(FilesView::isProtectedPath({}));
    }

    void legacyUpgradeRemovesOnlyEmptyDefaultsOnce()
    {
        QTemporaryDir temporary(QDir::current().filePath("legacy-files-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(temporary.path()); CTEST_CHECK(drive);
        for (const auto &name : {"Documents", "Audios", "3D objects", "Custom"})
            CTEST_CHECK(QDir().mkdir(temporary.filePath("Files/" + QString(name))));
        QFile keep(temporary.filePath("Files/Documents/.keep"));
        CTEST_CHECK(keep.open(QIODevice::WriteOnly)); keep.write("preserve hidden data"); keep.close();
        QFile manifest(temporary.filePath(".society-drive.json"));
        CTEST_CHECK(manifest.open(QIODevice::ReadOnly));
        auto data = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
        data.remove("filesLayoutVersion");
        CTEST_CHECK(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        manifest.write(QJsonDocument(data).toJson()); manifest.close();
        const auto reopened = SocietyDrive::open(temporary.path()); CTEST_CHECK(reopened);
        CTEST_COMPARE(reopened->identifier(), drive->identifier());
        CTEST_CHECK(keep.open(QIODevice::ReadOnly)); CTEST_COMPARE(keep.readAll(), "preserve hidden data"); keep.close();
        CTEST_CHECK(QFileInfo(temporary.filePath("Files/Custom")).isDir());
        CTEST_CHECK(!QFileInfo::exists(temporary.filePath("Files/Audios")));
        CTEST_CHECK(!QFileInfo::exists(temporary.filePath("Files/3D objects")));
        CTEST_CHECK(manifest.open(QIODevice::ReadOnly));
        CTEST_COMPARE(QJsonDocument::fromJson(manifest.readAll()).object().value("filesLayoutVersion").toInt(), 1);
        manifest.close();
        CTEST_CHECK(QDir().mkdir(temporary.filePath("Files/Audios")));
        CTEST_CHECK(SocietyDrive::open(temporary.path()));
        CTEST_CHECK(QFileInfo(temporary.filePath("Files/Audios")).isDir());
    }

    void legacyReplicasDeferCleanupUntilCompletion()
    {
        QTemporaryDir temporary(QDir::current().filePath("legacy-files-replica-XXXXXX.societycontainer"));
        const auto initial = SocietyDrive::create(temporary.path()); CTEST_CHECK(initial);
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        CTEST_CHECK(SocietyDrive::adoptReplicaIdentity(temporary.path(), initial->identifier(), host));
        CTEST_CHECK(QDir().mkdir(temporary.filePath("Files/Audios")));
        QFile manifest(temporary.filePath(".society-drive.json"));
        CTEST_CHECK(manifest.open(QIODevice::ReadOnly));
        auto data = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
        data.remove("filesLayoutVersion");
        CTEST_CHECK(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        manifest.write(QJsonDocument(data).toJson()); manifest.close();
        CTEST_CHECK(!SocietyDrive::open(temporary.path())->isReady());
        CTEST_CHECK(QFileInfo(temporary.filePath("Files/Audios")).isDir());
        CTEST_CHECK(SocietyDrive::completeReplica(temporary.path(), host));
        CTEST_CHECK(FilesView::open(temporary.path())->entries().isEmpty());
        CTEST_CHECK(QDir().mkdir(temporary.filePath("Files/Audios")));
        CTEST_CHECK(SocietyDrive::open(temporary.path()));
        CTEST_CHECK(QFileInfo(temporary.filePath("Files/Audios")).isDir());
    }

    void legacyUpgradePreservesFilesAndRedirectedEntries()
    {
        QTemporaryDir temporary(QDir::current().filePath("legacy-file-names-XXXXXX.societycontainer"));
        CTEST_CHECK(SocietyDrive::create(temporary.path()));
        QFile file(temporary.filePath("Files/Documents"));
        CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("ordinary file"); file.close();
#ifndef Q_OS_WIN
        CTEST_CHECK(createTestSymbolicLink(temporary.filePath("Models"), temporary.filePath("Files/Audios")));
#endif
        QFile manifest(temporary.filePath(".society-drive.json"));
        CTEST_CHECK(manifest.open(QIODevice::ReadOnly));
        auto data = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
        data.remove("filesLayoutVersion");
        CTEST_CHECK(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        manifest.write(QJsonDocument(data).toJson()); manifest.close();
        CTEST_CHECK(SocietyDrive::open(temporary.path()));
        CTEST_CHECK(file.open(QIODevice::ReadOnly)); CTEST_COMPARE(file.readAll(), "ordinary file");
#ifndef Q_OS_WIN
        CTEST_CHECK(QFileInfo(temporary.filePath("Files/Audios")).isSymLink());
        CTEST_CHECK(FilesView::open(temporary.path())->resolve("Audios").isEmpty());
#endif
    }

    void projectsFilesAndPreservesPrivateSections()
    {
        QTemporaryDir temporary(QDir::current().filePath("files-view-XXXXXX.societycontainer"));
        auto drive = SocietyDrive::create(temporary.path());
        CTEST_CHECK(drive);
        QFile privateFile(temporary.filePath("Models/private.safetensor"));
        CTEST_CHECK(privateFile.open(QIODevice::WriteOnly));
        privateFile.write("private model"); privateFile.close();
        QFile publicFile(temporary.filePath("Files/document.txt"));
        CTEST_CHECK(publicFile.open(QIODevice::WriteOnly));
        publicFile.write("public document"); publicFile.close();
        auto view = FilesView::open(temporary.path());
        CTEST_CHECK(view);
        CTEST_COMPARE(view->rootPath(), drive->sectionPath(StoreSection::Files));
        CTEST_COMPARE(view->resolve({}), view->rootPath());
        const auto children = view->entries();
        CTEST_COMPARE(children.size(), 1);
        CTEST_CHECK(std::any_of(children.begin(), children.end(), [](const auto &entry) { return entry.fileName() == "document.txt"; }));
        for (const auto section : allStoreSections())
            CTEST_CHECK(view->resolve(storeSectionName(section)).isEmpty());
        CTEST_CHECK(view->resolve(".society-drive.json").isEmpty());
        CTEST_COMPARE(view->resolve("document.txt"), publicFile.fileName());
        QFile throughView(view->resolve("created.txt", true));
        CTEST_CHECK(throughView.open(QIODevice::WriteOnly));
        throughView.write("created through the drive"); throughView.close();
        CTEST_CHECK(QFileInfo::exists(temporary.filePath("Files/created.txt")));
        CTEST_CHECK(!QFileInfo::exists(temporary.filePath("created.txt")));
        CTEST_CHECK(privateFile.open(QIODevice::ReadOnly));
        CTEST_COMPARE(privateFile.readAll(), "private model");
    }

    void rejectsPaths(const QString& path)
    {
        QTemporaryDir temporary(QDir::current().filePath("files-view-XXXXXX.societycontainer"));
        CTEST_CHECK(SocietyDrive::create(temporary.path()));
        auto view = FilesView::open(temporary.path());
        CTEST_CHECK(view);
        QString error;
        CTEST_CHECK(view->resolve(path, true, &error).isEmpty());
        CTEST_CHECK(!error.isEmpty());
    }
    void nestedNamesAndHiddenFiles()
    {
        QTemporaryDir temporary(QDir::current().filePath("files-view-XXXXXX.societycontainer"));
        CTEST_CHECK(SocietyDrive::create(temporary.path()));
        CTEST_CHECK(QDir().mkpath(temporary.filePath("Files/한글 folder/.hidden")));
        auto view = FilesView::open(temporary.path());
        CTEST_CHECK(view);
        CTEST_COMPARE(view->resolve("한글 folder/.hidden/new.txt", true), temporary.filePath("Files/한글 folder/.hidden/new.txt"));
        CTEST_CHECK(view->resolve("missing/new.txt", true).isEmpty());
        CTEST_COMPARE(view->entries("한글 folder").size(), 1);
    }
    void rejectsRedirectionAndDriveReplacement()
    {
        QTemporaryDir temporary(QDir::current().filePath("files-view-XXXXXX.societycontainer"));
        CTEST_CHECK(SocietyDrive::create(temporary.path()));
        auto view = FilesView::open(temporary.path());
        CTEST_CHECK(view);
#ifndef Q_OS_WIN
        CTEST_CHECK(createTestSymbolicLink(temporary.filePath("Models"), temporary.filePath("Files/private-link")));
        CTEST_CHECK(view->resolve("private-link", true).isEmpty());
        CTEST_CHECK(view->resolve("private-link/model", true).isEmpty());
        CTEST_COMPARE(view->entries().size(), 0);
#endif
        CTEST_CHECK(QFile::remove(temporary.filePath(".society-drive.json")));
        CTEST_CHECK(SocietyDrive::create(temporary.path()));
        CTEST_CHECK(!view->isValid());
        CTEST_CHECK(view->resolve("new", true).isEmpty());
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name" << std::endl; return 1; }
    const std::string_view selection = argc == 2 ? argv[1] : "";
    const bool allRows = selection == "rejectsPaths";
    society_ctest::CTestRunner runner(allRows ? std::string_view{} : selection);
    FilesViewTests tests;
    if (!allRows)
        runner.run("filesStartEmptyAndStayEmptyAcrossReopenAndReplicaCompletion", [&] { tests.filesStartEmptyAndStayEmptyAcrossReopenAndReplicaCompletion(); });
    if (!allRows)
        runner.run("formerDefaultNamesAreOrdinaryUserEntries", [&] { tests.formerDefaultNamesAreOrdinaryUserEntries(); });
    if (!allRows)
        runner.run("legacyUpgradeRemovesOnlyEmptyDefaultsOnce", [&] { tests.legacyUpgradeRemovesOnlyEmptyDefaultsOnce(); });
    if (!allRows)
        runner.run("legacyReplicasDeferCleanupUntilCompletion", [&] { tests.legacyReplicasDeferCleanupUntilCompletion(); });
    if (!allRows)
        runner.run("legacyUpgradePreservesFilesAndRedirectedEntries", [&] { tests.legacyUpgradePreservesFilesAndRedirectedEntries(); });
    if (!allRows)
        runner.run("projectsFilesAndPreservesPrivateSections", [&] { tests.projectsFilesAndPreservesPrivateSections(); });
    runner.run("rejectsPaths:../Models/private", [&] { tests.rejectsPaths(QString::fromUtf8("../Models/private")); });
    runner.run("rejectsPaths:folder/../../Models", [&] { tests.rejectsPaths(QString::fromUtf8("folder/../../Models")); });
    runner.run("rejectsPaths:/Models", [&] { tests.rejectsPaths(QString::fromUtf8("/Models")); });
    runner.run("rejectsPaths:C:/Models", [&] { tests.rejectsPaths(QString::fromUtf8("C:/Models")); });
    runner.run("rejectsPaths:folder\\name", [&] { tests.rejectsPaths(QString::fromUtf8("folder\\name")); });
    runner.run("rejectsPaths:a//b", [&] { tests.rejectsPaths(QString::fromUtf8("a//b")); });
    runner.run("rejectsPaths:a/./b", [&] { tests.rejectsPaths(QString::fromUtf8("a/./b")); });
    runner.run("rejectsPaths:a/../b", [&] { tests.rejectsPaths(QString::fromUtf8("a/../b")); });
    runner.run("rejectsPaths:a/", [&] { tests.rejectsPaths(QString::fromUtf8("a/")); });
    runner.run("rejectsPaths:.", [&] { tests.rejectsPaths(QString::fromUtf8(".")); });
    runner.run("rejectsPaths:..", [&] { tests.rejectsPaths(QString::fromUtf8("..")); });
    runner.run("rejectsPaths:nul", [&] { tests.rejectsPaths(QString("bad") + QChar::Null + "name"); });
    if (!allRows)
        runner.run("nestedNamesAndHiddenFiles", [&] { tests.nestedNamesAndHiddenFiles(); });
    if (!allRows)
        runner.run("rejectsRedirectionAndDriveReplacement", [&] { tests.rejectsRedirectionAndDriveReplacement(); });
    return runner.result();
}
