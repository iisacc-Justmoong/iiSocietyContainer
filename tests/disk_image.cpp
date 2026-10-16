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
#include <QTest>
#include <QScopeGuard>
#include <QProcess>
#include <sys/mount.h>
#include <unistd.h>

using namespace iiSocietyContainer;
namespace fs = std::filesystem;
static fs::path native(const QString &path) { return path.toStdString(); }
static QString qt(const fs::path &path) { return QString::fromStdString(path.string()); }

class DiskImageTests : public QObject {
    Q_OBJECT
private slots:
    void ordinaryPackageLookupsDoNotPollDiskImages() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_DIRECTORY "/disk-lookup-XXXXXX"));
        QVERIFY(fixture.isValid());
        const auto package = fixture.filePath("Source.societycontainer");
        QVERIFY(QDir().mkdir(package));
        QElapsedTimer elapsed;
        elapsed.start();
        for (int index = 0; index < 128; ++index)
            QVERIFY(!DiskImage::mountedAt(native(package)));
        // A directory beneath an existing volume cannot itself be an image's
        // mount point. Repeated consumer validation must stay on the statfs path.
        QVERIFY2(elapsed.elapsed() < 1500, "Ordinary directory lookups polled the system disk-image tool.");
    }
    void savedImageSettingsNeverMountOrRenameThePackage() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_DIRECTORY "/saved-disk-XXXXXX"));
        QVERIFY(fixture.isValid());
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
        QVERIFY(QDir().mkpath(legacy + "/bands"));
        QFile marker(legacy + "/Society.volume"); QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.write("iisacc.society.disk-image/1\n"); marker.close();
        const QByteArray settingsBytes = QJsonDocument(QJsonObject{{"schemaVersion", 2},
            {"imagePath", renamed}, {"path", fixture.filePath("unmounted-root")},
            {"containerId", "saved-container-id"}}).toJson();
        QFile settings(settingsPath); QVERIFY(settings.open(QIODevice::WriteOnly));
        QCOMPARE(settings.write(settingsBytes), settingsBytes.size()); settings.close();
        QString error;
        QVERIFY(!SharedStorage::open({}, &error));
        QVERIFY(error.contains("not restored"));
        QVERIFY(QFileInfo::exists(legacy + "/Society.volume"));
        QVERIFY(!QFileInfo::exists(renamed));
        QVERIFY(!QFileInfo::exists(fixture.filePath("unmounted-root")));
        QVERIFY(settings.open(QIODevice::ReadOnly)); QCOMPARE(settings.readAll(), settingsBytes);
    }
    void mountedImageCannotReplaceOrdinaryFolderSettings() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_DIRECTORY "/image-selection-XXXXXX"));
        QVERIFY(fixture.isValid());
        const auto previousSettings = qgetenv("SOCIETY_STORAGE_SETTINGS_PATH");
        const auto previousContainer = qgetenv("SOCIETY_CONTAINER_PATH");
        const auto cleanupSettings = qScopeGuard([&] {
            qputenv("SOCIETY_STORAGE_SETTINGS_PATH", previousSettings);
            qputenv("SOCIETY_CONTAINER_PATH", previousContainer);
        });
        const auto settingsPath = fixture.filePath("storage.json");
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", settingsPath.toUtf8());
        qunsetenv("SOCIETY_CONTAINER_PATH");
        const auto ordinary = SocietyDrive::createAt(fixture.path()); QVERIFY(ordinary);
        QVERIFY(SharedStorage::setDefaultContainer(ordinary->rootPath()));
        QFile settings(settingsPath); QVERIFY(settings.open(QIODevice::ReadOnly));
        const auto settingsBytes = settings.readAll(); settings.close();
        const auto imageParent = fixture.filePath("image-parent"); QVERIFY(QDir().mkdir(imageParent));
        const auto volume = DiskImage::create(native(imageParent), 512ULL * 1024 * 1024);
        QVERIFY2(volume, volume ? "" : volume.error().c_str());
        const auto cleanupImage = qScopeGuard([&] { DiskImage::detach(volume->imagePath); });
        QString error;
        QVERIFY(!SocietyDrive::create(qt(volume->mountPath), &error));
        QVERIFY(error.contains("Legacy"));
        QVERIFY(!SharedStorage::setDefaultContainer(qt(volume->mountPath), &error));
        QVERIFY(error.contains("Legacy"));
        QVERIFY(!SharedStorage::open(qt(volume->mountPath), &error));
        QVERIFY(error.contains("not restored"));
        QVERIFY(settings.open(QIODevice::ReadOnly)); QCOMPARE(settings.readAll(), settingsBytes);
        const auto reopened = SharedStorage::open(); QVERIFY(reopened);
        QCOMPARE(reopened->drive().identifier(), ordinary->identifier());
        QCOMPARE(reopened->drive().rootPath(), ordinary->rootPath());
    }
    void lowLevelImageMigrationNeverMakesItAManagedPackage() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_DIRECTORY "/legacy-disk-XXXXXX")); QVERIFY(fixture.isValid());
        const auto image = fixture.filePath("Society.sparsebundle");
        QCOMPARE(QProcess::execute("/usr/bin/hdiutil", {"create", "-megabytes", "512", "-type", "SPARSEBUNDLE", "-fs", "APFS", "-volname", "Society Legacy Test", image}), 0);
        QFile marker(image + "/Society.volume"); QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.write("iisacc.society.disk-image/1\n"); marker.close();
        const auto root = "/Volumes/Society-" + QFileInfo(fixture.path()).fileName();
        QCOMPARE(QProcess::execute("/usr/bin/hdiutil", {"attach", image, "-noautoopen", "-nobrowse", "-mountpoint", root}), 0);
        const auto cleanup = qScopeGuard([&] { DiskImage::detach(native(image)); });
        QVERIFY(DiskImage::mountedAt(native(root)));
        const auto sourcePath = fixture.filePath("Source.societycontainer"); QVERIFY(QDir().mkdir(sourcePath));
        const auto source = SocietyDrive::create(sourcePath); QVERIFY(source);
        for (const auto section : allStoreSections()) QVERIFY(QDir().mkdir(root + '/' + storeSectionName(section)));
        QVERIFY(QFile::copy(source->rootPath() + "/.society-drive.json", root + "/.society-drive.json"));
        QFile file(root + "/Files/preserved.txt"); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("legacy bytes"); file.close();
        QFile model(root + "/Models/private.txt"); QVERIFY(model.open(QIODevice::WriteOnly)); model.write("app only"); model.close();
        const auto disk = DiskImage::mount(native(image)); QVERIFY2(disk, disk ? "" : disk.error().c_str());
        QVERIFY(!SocietyDrive::open(qt(disk->mountPath)));
        QVERIFY(!SocietyDrive::create(qt(disk->mountPath)));
        const auto filesPath = DiskImage::filesRoot(disk->mountPath); QVERIFY(filesPath);
        const auto files = qt(*filesPath);
        QFile publicFile(files + "/preserved.txt"); QVERIFY(publicFile.open(QIODevice::ReadOnly));
        QCOMPARE(publicFile.readAll(), QByteArray("legacy bytes")); publicFile.close();
        QVERIFY(!QFileInfo::exists(files + "/Models")); QVERIFY(!QFileInfo::exists(files + "/Files"));
        QVERIFY(QFileInfo::exists(qt(disk->mountPath) + "/Models/private.txt"));
        QVERIFY(QFileInfo::exists(qt(disk->mountPath) + "/.society-legacy-files/preserved.txt"));
        QVERIFY(DiskImage::detach(native(image))); QVERIFY(DiskImage::mount(native(image)));
    }
    void preservesConflictingImagesAndRejectsOrdinaryFolders() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_DIRECTORY "/disk-conflict-XXXXXX"));
        QVERIFY(fixture.isValid());
        QVERIFY(!DiskImage::create("relative"));
        QVERIFY(!DiskImage::mount(native(fixture.path())));
        QFile existing(fixture.filePath("Society.sparsebundle"));
        QVERIFY(existing.open(QIODevice::WriteOnly)); existing.write("do not replace"); existing.close();
        QVERIFY(!DiskImage::create(native(fixture.path()), 512ULL * 1024 * 1024));
        QVERIFY(existing.open(QIODevice::ReadOnly)); QCOMPARE(existing.readAll(), QByteArray("do not replace"));
    }
};
QTEST_GUILESS_MAIN(DiskImageTests)
#include "disk_image.moc"
