#include <DiskImage.h>
#include <SharedStorage.h>
#include <StorageMap.h>
#include <FilesView.h>
#include <FileOperations.h>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QScopeGuard>
#include <QProcess>
#include <sys/mount.h>

using namespace iiSocietyContainer;
namespace fs = std::filesystem;
static fs::path native(const QString &path) { return path.toStdString(); }
static QString qt(const fs::path &path) { return QString::fromStdString(path.string()); }

class DiskImageTests : public QObject {
    Q_OBJECT
private slots:
    void createsAnActualDiskAndRestoresTheSameContainerAfterEject() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_DIRECTORY "/disk-image-XXXXXX"));
        QVERIFY(fixture.isValid());
        const auto parent = fixture.filePath(QString::fromUtf8("storage 한글 #100%"));
        QVERIFY(QDir().mkpath(parent));
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", fixture.filePath("storage.json").toUtf8());
        QFile sentinel(parent + "/existing.txt"); QVERIFY(sentinel.open(QIODevice::WriteOnly));
        sentinel.write("untouched"); sentinel.close();
        const auto result = DiskImage::create(native(parent), 512ULL * 1024 * 1024);
        QVERIFY2(result, result ? "" : result.error().c_str());
        const auto image = result->imagePath;
        const auto cleanup = qScopeGuard([&] { DiskImage::detach(image); });
        QVERIFY(result->mountPath != native(parent));
        QVERIFY(result->device.starts_with("/dev/disk"));
        const QStorageInfo volume(qt(result->mountPath));
        QVERIFY(volume.isReady()); QVERIFY(!volume.isReadOnly());
        QCOMPARE(volume.fileSystemType(), QByteArray("apfs"));
        QCOMPARE(volume.device(), QByteArray::fromStdString(result->device));
        QVERIFY(volume.device() != QStorageInfo(parent).device());
        struct statfs privateMount{};
        QVERIFY(!statfs(result->mountPath.c_str(), &privateMount));
        QVERIFY(privateMount.f_flags & MNT_DONTBROWSE);
        QVERIFY(volume.bytesTotal() >= 400LL * 1024 * 1024);
        QVERIFY2(volume.bytesTotal() <= 512LL * 1024 * 1024, "Disk capacity must use bytes, not 512-byte sectors.");
        QString error;
        const auto drive = SocietyDrive::create(qt(result->mountPath), &error);
        QVERIFY2(drive, qPrintable(error));
        const auto publicRoot = drive->sectionPath(StoreSection::Files);
        QCOMPARE(QStorageInfo(publicRoot).rootPath(), publicRoot);
        struct statfs publicMount{};
        QVERIFY(!statfs(native(publicRoot).c_str(), &publicMount));
        QVERIFY(!(publicMount.f_flags & MNT_DONTBROWSE));
        QVERIFY(publicRoot != drive->rootPath());
        for (const auto section : allStoreSections()) {
            if (section != StoreSection::Files)
                QVERIFY(!QFileInfo::exists(QDir(publicRoot).filePath(storeSectionName(section))));
        }
        QVERIFY(!QFileInfo::exists(publicRoot + "/Files"));
        QVERIFY(!QFileInfo::exists(publicRoot + "/.society-drive.json"));
        QVERIFY(QDir(publicRoot).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(QDir().mkdir(publicRoot + "/Documents"));
        QCOMPARE(drive->relativePath(publicRoot + "/Documents/item.txt"), QString("Files/Documents/item.txt"));
        QCOMPARE(drive->resolvePath("Files/Documents/item.txt"), publicRoot + "/Documents/item.txt");
        QCOMPARE(StorageMap(*drive).localPath("files/Documents/item.txt"), publicRoot + "/Documents/item.txt");
        QCOMPARE(drive->sectionForPath(publicRoot), std::optional(StoreSection::Files));
        QVERIFY(FileOperations::containingDrive(publicRoot));
        QVERIFY(!SocietyDrive::create(publicRoot));
        QVERIFY(!SocietyDrive::open(publicRoot));
        QVERIFY(SharedStorage::setDefaultContainer(drive->rootPath(), &error));
        const auto id = drive->identifier();
        QFile payload(drive->sectionPath(StoreSection::Files) + "/persist.txt");
        QVERIFY(payload.open(QIODevice::WriteOnly)); payload.write("persistent volume data"); payload.close();
        QFile discarded(publicRoot + "/discard.txt"); QVERIFY(discarded.open(QIODevice::WriteOnly));
        discarded.write("recoverable private content"); discarded.close();
        const auto trashed = FileOperations(*drive).perform(FileOperations::Action::Trash, discarded.fileName());
        QVERIFY2(trashed.error.isEmpty(), qPrintable(trashed.error));
        QVERIFY(!QFileInfo::exists(discarded.fileName()));
        QFile recovered(trashed.path); QVERIFY(recovered.open(QIODevice::ReadOnly));
        QCOMPARE(recovered.readAll(), QByteArray("recoverable private content")); recovered.close();
        QVERIFY(!QFileInfo::exists(parent + "/Files"));
        QVERIFY(!QFileInfo::exists(parent + "/.society-drive.json"));
        QFile settings(fixture.filePath("storage.json")); QVERIFY(settings.open(QIODevice::ReadOnly));
        const auto config = QJsonDocument::fromJson(settings.readAll()).object(); settings.close();
        QCOMPARE(config.value("schemaVersion").toInt(), 2);
        QCOMPARE(config.value("imagePath").toString(), qt(image));
        QVERIFY(DiskImage::create(native(parent), 512ULL * 1024 * 1024));
        QCOMPARE(QProcess::execute("/usr/sbin/diskutil", {"renameVolume", qt(result->mountPath), "Society Remount Test"}), 0);
        QVERIFY(DiskImage::detach(image));
        QVERIFY(!DiskImage::mountedAt(result->mountPath));
        const auto reopened = SharedStorage::open({}, &error);
        QVERIFY2(reopened, qPrintable(error));
        QCOMPARE(reopened->drive().identifier(), id);
        QVERIFY(reopened->drive().rootPath() != qt(result->mountPath));
        QFile restored(reopened->drive().sectionPath(StoreSection::Files) + "/persist.txt");
        QVERIFY(restored.open(QIODevice::ReadOnly)); QCOMPARE(restored.readAll(), QByteArray("persistent volume data"));
        restored.close();
        QVERIFY(sentinel.open(QIODevice::ReadOnly)); QCOMPARE(sentinel.readAll(), QByteArray("untouched")); sentinel.close();
        QVERIFY(!DiskImage::create(native(reopened->drive().sectionPath(StoreSection::Files)), 512ULL * 1024 * 1024));
        QVERIFY(DiskImage::detach(image));
        fs::rename(image, image.parent_path() / "offline.sparsebundle");
        QVERIFY(!SharedStorage::open({}, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!fs::exists(image)); // A missing disk must never become an empty replacement.
        fs::rename(image.parent_path() / "offline.sparsebundle", image);
    }
    void migratesLegacyFilesWithoutPublishingInternalDirectories() {
        QTemporaryDir fixture(QStringLiteral(SOCIETY_TEST_DIRECTORY "/legacy-disk-XXXXXX")); QVERIFY(fixture.isValid());
        const auto image = fixture.filePath("Society.sparsebundle");
        QCOMPARE(QProcess::execute("/usr/bin/hdiutil", {"create", "-megabytes", "512", "-type", "SPARSEBUNDLE", "-fs", "APFS", "-volname", "Society Legacy Test", image}), 0);
        QFile marker(image + "/Society.volume"); QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.write("iisacc.society.disk-image/1\n"); marker.close();
        const auto root = "/Volumes/Society-" + QFileInfo(fixture.path()).fileName();
        QCOMPARE(QProcess::execute("/usr/bin/hdiutil", {"attach", image, "-noautoopen", "-nobrowse", "-mountpoint", root}), 0);
        const auto cleanup = qScopeGuard([&] { DiskImage::detach(native(image)); });
        QVERIFY(DiskImage::mountedAt(native(root)));
        const auto legacy = SocietyDrive::create(root); QVERIFY(legacy);
        QFile file(root + "/Files/preserved.txt"); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("legacy bytes"); file.close();
        QFile model(root + "/Models/private.txt"); QVERIFY(model.open(QIODevice::WriteOnly)); model.write("app only"); model.close();
        const auto disk = DiskImage::mount(native(image)); QVERIFY2(disk, disk ? "" : disk.error().c_str());
        const auto migrated = SocietyDrive::open(qt(disk->mountPath)); QVERIFY(migrated);
        QCOMPARE(migrated->identifier(), legacy->identifier());
        const auto files = migrated->sectionPath(StoreSection::Files);
        QFile publicFile(files + "/preserved.txt"); QVERIFY(publicFile.open(QIODevice::ReadOnly));
        QCOMPARE(publicFile.readAll(), QByteArray("legacy bytes")); publicFile.close();
        QVERIFY(!QFileInfo::exists(files + "/Models")); QVERIFY(!QFileInfo::exists(files + "/Files"));
        QVERIFY(QFileInfo::exists(migrated->rootPath() + "/Models/private.txt"));
        QVERIFY(QFileInfo::exists(migrated->rootPath() + "/.society-legacy-files/preserved.txt"));
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
