#include <DiskImage.h>
#include <SharedStorage.h>
#include <StorageMap.h>
#include <FilesView.h>
#include <FileOperations.h>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QStorageInfo>
#include <QTemporaryDir>
#include "CTestSupport.h"
#include <QCoreApplication>
#include <QEvent>
#include <QScopeGuard>
#include <QProcess>
#include <atomic>
#include <barrier>
#include <dlfcn.h>
#include <sys/mount.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace iiSocietyContainer;
namespace fs = std::filesystem;
static fs::path native(const QString &path) { return path.toStdString(); }
static QString qt(const fs::path &path) { return QString::fromStdString(path.string()); }

class DiskImageTests : public QObject {
public:
    void libraryDoesNotImportSharedMountBuffer() {
        Dl_info library{};
        CTEST_CHECK(dladdr(reinterpret_cast<const void *>(&DiskImage::supported), &library));
        CTEST_CHECK(library.dli_fname);
        QProcess nm;
        nm.start("/usr/bin/nm", {"-u", QString::fromUtf8(library.dli_fname)});
        CTEST_CHECK_MESSAGE(nm.waitForFinished(60000), qPrintable(nm.errorString()));
        CTEST_COMPARE(nm.exitStatus(), QProcess::NormalExit);
        CTEST_COMPARE(nm.exitCode(), 0);
        for (const auto &line : nm.readAllStandardOutput().split('\n')) {
            const auto symbol = line.trimmed();
            CTEST_CHECK_MESSAGE(symbol != "_getmntinfo" && symbol != "_getmntinfo$INODE64",
                     "The container library imports getmntinfo's process-wide shared buffer.");
        }
    }
    void concurrentContainerLookupCoexistsWithOtherMountQueries() {
        constexpr int workerCount = 6;
        constexpr int lookupsPerWorker = 40;
        const auto expected = DiskImage::containerRoot("/");
        std::barrier start(workerCount + 1);
        std::atomic<bool> done{false};
        std::atomic<int> externalQueries{0};
        std::atomic<int> inconsistent{0};
        std::jthread external([&] {
            start.arrive_and_wait();
            while (!done.load()) {
                struct statfs *mounts = nullptr;
                (void)getmntinfo(&mounts, MNT_NOWAIT);
                ++externalQueries;
                std::this_thread::yield();
            }
        });
        std::vector<std::jthread> workers;
        workers.reserve(workerCount);
        for (int i = 0; i < workerCount; ++i) {
            workers.emplace_back([&] {
                start.arrive_and_wait();
                while (externalQueries.load() == 0) std::this_thread::yield();
                for (int n = 0; n < lookupsPerWorker; ++n)
                    if (DiskImage::containerRoot("/") != expected) ++inconsistent;
            });
        }
        workers.clear();
        done = true;
        external.join();
        CTEST_CHECK(externalQueries.load() > 0);
        CTEST_COMPARE(inconsistent.load(), 0);
    }
    void ordinaryPackageLookupsDoNotPollDiskImages() {
        QTemporaryDir fixture((QDir::currentPath() + QStringLiteral("/disk-lookup-XXXXXX")));
        CTEST_CHECK(fixture.isValid());
        const auto package = fixture.filePath("Source.societycontainer");
        CTEST_CHECK(QDir().mkdir(package));
        QElapsedTimer elapsed;
        elapsed.start();
        for (int index = 0; index < 128; ++index)
            CTEST_CHECK(!DiskImage::mountedAt(native(package)));
        // A directory beneath an existing volume cannot itself be an image's
        // mount point. Repeated consumer validation must stay on the statfs path.
        CTEST_CHECK_MESSAGE(elapsed.elapsed() < 1500, "Ordinary directory lookups polled the system disk-image tool.");
    }
    void savedImageSettingsNeverMountOrRenameThePackage() {
        QTemporaryDir fixture((QDir::currentPath() + QStringLiteral("/saved-disk-XXXXXX")));
        CTEST_CHECK(fixture.isValid());
        const auto previousSettings = qgetenv("SOCIETY_STORAGE_SETTINGS_PATH");
        const auto previousContainer = qgetenv("SOCIETY_CONTAINER_PATH");
        const auto cleanup = qScopeGuard([&] {
            qputenv("SOCIETY_STORAGE_SETTINGS_PATH", previousSettings);
            qputenv("SOCIETY_CONTAINER_PATH", previousContainer);
        });
        const auto settingsPath = fixture.filePath("storage.json");
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", settingsPath.toUtf8());
        qunsetenv("SOCIETY_CONTAINER_PATH");
        const auto legacy = fixture.filePath("Society.sparsebundle");
        const auto renamed = fixture.filePath("Society.societycontainer");
        CTEST_CHECK(QDir().mkpath(legacy + "/bands"));
        QFile marker(legacy + "/Society.volume"); CTEST_CHECK(marker.open(QIODevice::WriteOnly));
        marker.write("iisacc.society.disk-image/1\n"); marker.close();
        const QByteArray settingsBytes = QJsonDocument(QJsonObject{{"schemaVersion", 2},
            {"imagePath", renamed}, {"path", fixture.filePath("unmounted-root")},
            {"containerId", "saved-container-id"}}).toJson();
        QFile settings(settingsPath); CTEST_CHECK(settings.open(QIODevice::WriteOnly));
        CTEST_COMPARE(settings.write(settingsBytes), settingsBytes.size()); settings.close();
        QString error;
        CTEST_CHECK(!SharedStorage::open({}, &error));
        CTEST_CHECK(error.contains("not restored"));
        CTEST_CHECK(QFileInfo::exists(legacy + "/Society.volume"));
        CTEST_CHECK(!QFileInfo::exists(renamed));
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("unmounted-root")));
        CTEST_CHECK(settings.open(QIODevice::ReadOnly)); CTEST_COMPARE(settings.readAll(), settingsBytes);
    }
    void mountedImageCannotReplaceOrdinaryFolderSettings() {
        QTemporaryDir fixture((QDir::currentPath() + QStringLiteral("/image-selection-XXXXXX")));
        CTEST_CHECK(fixture.isValid());
        const auto previousSettings = qgetenv("SOCIETY_STORAGE_SETTINGS_PATH");
        const auto previousContainer = qgetenv("SOCIETY_CONTAINER_PATH");
        const auto cleanupSettings = qScopeGuard([&] {
            qputenv("SOCIETY_STORAGE_SETTINGS_PATH", previousSettings);
            qputenv("SOCIETY_CONTAINER_PATH", previousContainer);
        });
        const auto settingsPath = fixture.filePath("storage.json");
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", settingsPath.toUtf8());
        qunsetenv("SOCIETY_CONTAINER_PATH");
        const auto ordinary = SocietyDrive::createAt(fixture.path()); CTEST_CHECK(ordinary);
        CTEST_CHECK(SharedStorage::setDefaultContainer(ordinary->rootPath()));
        QFile settings(settingsPath); CTEST_CHECK(settings.open(QIODevice::ReadOnly));
        const auto settingsBytes = settings.readAll(); settings.close();
        const auto imageParent = fixture.filePath("image-parent"); CTEST_CHECK(QDir().mkdir(imageParent));
        const auto volume = DiskImage::create(native(imageParent), 512ULL * 1024 * 1024);
        CTEST_CHECK_MESSAGE(volume, volume ? "" : volume.error().c_str());
        const auto cleanupImage = qScopeGuard([&] { DiskImage::detach(volume->imagePath); });
        QString error;
        CTEST_CHECK(!SocietyDrive::create(qt(volume->mountPath), &error));
        CTEST_CHECK(error.contains("Legacy"));
        CTEST_CHECK(!SharedStorage::setDefaultContainer(qt(volume->mountPath), &error));
        CTEST_CHECK(error.contains("Legacy"));
        CTEST_CHECK(!SharedStorage::open(qt(volume->mountPath), &error));
        CTEST_CHECK(error.contains("not restored"));
        CTEST_CHECK(settings.open(QIODevice::ReadOnly)); CTEST_COMPARE(settings.readAll(), settingsBytes);
        const auto reopened = SharedStorage::open(); CTEST_CHECK(reopened);
        CTEST_COMPARE(reopened->drive().identifier(), ordinary->identifier());
        CTEST_COMPARE(reopened->drive().rootPath(), ordinary->rootPath());
    }
    void lowLevelImageMigrationNeverMakesItAManagedPackage() {
        QTemporaryDir fixture((QDir::currentPath() + QStringLiteral("/legacy-disk-XXXXXX"))); CTEST_CHECK(fixture.isValid());
        const auto image = fixture.filePath("Society.sparsebundle");
        CTEST_COMPARE(QProcess::execute("/usr/bin/hdiutil", {"create", "-megabytes", "512", "-type", "SPARSEBUNDLE", "-fs", "APFS", "-volname", "Society Legacy Test", image}), 0);
        QFile marker(image + "/Society.volume"); CTEST_CHECK(marker.open(QIODevice::WriteOnly));
        marker.write("iisacc.society.disk-image/1\n"); marker.close();
        const auto root = "/Volumes/Society-" + QFileInfo(fixture.path()).fileName();
        CTEST_COMPARE(QProcess::execute("/usr/bin/hdiutil", {"attach", image, "-noautoopen", "-nobrowse", "-mountpoint", root}), 0);
        const auto cleanup = qScopeGuard([&] { DiskImage::detach(native(image)); });
        CTEST_CHECK(DiskImage::mountedAt(native(root)));
        const auto sourcePath = fixture.filePath("Source.societycontainer"); CTEST_CHECK(QDir().mkdir(sourcePath));
        const auto source = SocietyDrive::create(sourcePath); CTEST_CHECK(source);
        for (const auto section : allStoreSections()) CTEST_CHECK(QDir().mkdir(root + '/' + storeSectionName(section)));
        CTEST_CHECK(QFile::copy(source->rootPath() + "/.society-drive.json", root + "/.society-drive.json"));
        QFile file(root + "/Files/preserved.txt"); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("legacy bytes"); file.close();
        QFile model(root + "/Models/private.txt"); CTEST_CHECK(model.open(QIODevice::WriteOnly)); model.write("app only"); model.close();
        const auto disk = DiskImage::mount(native(image)); CTEST_CHECK_MESSAGE(disk, disk ? "" : disk.error().c_str());
        CTEST_CHECK(!SocietyDrive::open(qt(disk->mountPath)));
        CTEST_CHECK(!SocietyDrive::create(qt(disk->mountPath)));
        const auto filesPath = DiskImage::filesRoot(disk->mountPath); CTEST_CHECK(filesPath);
        const auto files = qt(*filesPath);
        QFile publicFile(files + "/preserved.txt"); CTEST_CHECK(publicFile.open(QIODevice::ReadOnly));
        CTEST_COMPARE(publicFile.readAll(), QByteArray("legacy bytes")); publicFile.close();
        CTEST_CHECK(!QFileInfo::exists(files + "/Models")); CTEST_CHECK(!QFileInfo::exists(files + "/Files"));
        CTEST_CHECK(QFileInfo::exists(qt(disk->mountPath) + "/Models/private.txt"));
        CTEST_CHECK(QFileInfo::exists(qt(disk->mountPath) + "/.society-legacy-files/preserved.txt"));
        CTEST_CHECK(DiskImage::detach(native(image))); CTEST_CHECK(DiskImage::mount(native(image)));
    }
    void preservesConflictingImagesAndRejectsOrdinaryFolders() {
        QTemporaryDir fixture((QDir::currentPath() + QStringLiteral("/disk-conflict-XXXXXX")));
        CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(!DiskImage::create("relative"));
        CTEST_CHECK(!DiskImage::mount(native(fixture.path())));
        QFile existing(fixture.filePath("Society.sparsebundle"));
        CTEST_CHECK(existing.open(QIODevice::WriteOnly)); existing.write("do not replace"); existing.close();
        CTEST_CHECK(!DiskImage::create(native(fixture.path()), 512ULL * 1024 * 1024));
        CTEST_CHECK(existing.open(QIODevice::ReadOnly)); CTEST_COMPARE(existing.readAll(), QByteArray("do not replace"));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    society_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    DiskImageTests tests;
    runner.run("libraryDoesNotImportSharedMountBuffer", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.libraryDoesNotImportSharedMountBuffer(); });
    runner.run("concurrentContainerLookupCoexistsWithOtherMountQueries", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.concurrentContainerLookupCoexistsWithOtherMountQueries(); });
    runner.run("ordinaryPackageLookupsDoNotPollDiskImages", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.ordinaryPackageLookupsDoNotPollDiskImages(); });
    runner.run("savedImageSettingsNeverMountOrRenameThePackage", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.savedImageSettingsNeverMountOrRenameThePackage(); });
    runner.run("mountedImageCannotReplaceOrdinaryFolderSettings", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.mountedImageCannotReplaceOrdinaryFolderSettings(); });
    runner.run("lowLevelImageMigrationNeverMakesItAManagedPackage", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.lowLevelImageMigrationNeverMakesItAManagedPackage(); });
    runner.run("preservesConflictingImagesAndRejectsOrdinaryFolders", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.preservesConflictingImagesAndRejectsOrdinaryFolders(); });
    return runner.result();
}
