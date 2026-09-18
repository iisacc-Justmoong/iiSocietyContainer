#include "FileOperations.h"
#include "StorageMap.h"
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTest>
using namespace iiSocietyContainer;
class FileOperationsTests : public QObject {
    Q_OBJECT
private slots:
    void deletionBypassesContentAndReplicationWork() {
        QTemporaryDir temp(QString(SOCIETY_TEST_DIRECTORY) + "/direct-delete-XXXXXX"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        StorageMap map(*drive); FileOperations ops(*drive);
        const auto source = temp.filePath("Models/Checkpoint/large.safetensors");
        constexpr qint64 bytes = 8LL * 1024 * 1024 * 1024;
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.resize(bytes)); file.close();
        // A stale map must neither download nor prevent moving the actual file.
        const QJsonObject entry{{"path", "models/Checkpoint/large.safetensors"}, {"kind", "file"},
            {"size", "1"}, {"resident", false}, {"version", QString(64, 'a')}};
        QVERIFY(map.publish({entry}));
        QLockFile sync(temp.filePath(".society-sync/operation.lock")); QVERIFY(sync.tryLock());
        QElapsedTimer elapsed; elapsed.start();
        const auto moved = ops.perform(FileOperations::Action::Trash, source);
        QVERIFY2(moved, qPrintable(moved.error));
        qInfo() << "8 GiB trash rename (ms):" << elapsed.elapsed();
        QVERIFY(!QFileInfo::exists(source)); QCOMPARE(QFileInfo(moved.path).size(), bytes);
        auto deleted = entry; deleted["path"] = "deleted/large.safetensors";
        QVERIFY(map.publish({entry, deleted})); elapsed.restart();
        const auto removed = ops.perform(FileOperations::Action::Remove, moved.path);
        QVERIFY2(removed, qPrintable(removed.error));
        qInfo() << "8 GiB permanent removal (ms):" << elapsed.elapsed();
        QVERIFY(!QFileInfo::exists(moved.path)); QVERIFY(map.pendingRequests().isEmpty());
        QCOMPARE(map.objects(), QJsonArray({entry, deleted})); // No graph/catalog mutation in the deletion path.
    }
    void removesPartialPackagesWithoutResolvingMissingChildren() {
        QTemporaryDir temp(QString(SOCIETY_TEST_DIRECTORY) + "/partial-delete-XXXXXX"); QVERIFY(temp.isValid());
        QTemporaryDir outside(QString(SOCIETY_TEST_DIRECTORY) + "/delete-keep-XXXXXX"); QVERIFY(outside.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive); StorageMap map(*drive);
        QVERIFY(QDir().mkpath(temp.filePath("Models/partial/sub")));
        QFile keep(outside.filePath("keep.txt")); QVERIFY(keep.open(QIODevice::WriteOnly)); keep.write("keep"); keep.close();
        QVERIFY(QFile::copy(keep.fileName(), temp.filePath("Models/partial/sub/present.bin")));
        QVERIFY(QFile::link(outside.path(), temp.filePath("Models/partial/sub/link")));
        QVERIFY(map.publish({QJsonObject{{"path", "models/partial/missing.bin"}, {"kind", "file"}, {"resident", false}}}));
        FileOperations ops(*drive);
        const auto moved = ops.perform(FileOperations::Action::Trash, temp.filePath("Models/partial"));
        QVERIFY2(moved, qPrintable(moved.error));
        QVERIFY(QFileInfo::exists(moved.path + "/sub/present.bin"));
        QVERIFY(ops.perform(FileOperations::Action::Remove, moved.path));
        QVERIFY(!QFileInfo::exists(moved.path)); QVERIFY(QFileInfo::exists(keep.fileName()));
        QVERIFY(map.pendingRequests().isEmpty());
    }
    void handlesFilesPackagesAndCollisions() {
        QTemporaryDir temp(QString(SOCIETY_TEST_DIRECTORY) + "/file-actions-XXXXXX"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive);
        FileOperations ops(*drive);
        const auto path = temp.filePath("Models/model.safetensors");
        QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("model bytes"); f.close();
        auto first = ops.perform(FileOperations::Action::Duplicate, path); QVERIFY2(first, qPrintable(first.error));
        auto second = ops.perform(FileOperations::Action::Duplicate, path); QVERIFY(second); QVERIFY(first.path != second.path);
        QFile copy(first.path); QVERIFY(copy.open(QIODevice::ReadOnly)); QCOMPARE(copy.readAll(), QByteArray("model bytes")); copy.close();
        auto renamed = ops.perform(FileOperations::Action::Rename, first.path, "renamed.safetensors"); QVERIFY(renamed); QVERIFY(!QFileInfo::exists(first.path));
        QVERIFY(!ops.perform(FileOperations::Action::Rename, renamed.path, "model.safetensors"));
        QVERIFY(QDir().mkpath(temp.filePath("Models/package/sub")));
        QVERIFY(QFile::copy(path, temp.filePath("Models/package/sub/weight.bin")));
        auto package = ops.perform(FileOperations::Action::Duplicate, temp.filePath("Models/package")); QVERIFY(package);
        QVERIFY(QFileInfo::exists(package.path + "/sub/weight.bin"));
        auto trashed = ops.perform(FileOperations::Action::Trash, temp.filePath("Models/package")); QVERIFY(trashed);
        QVERIFY(trashed.path.startsWith(temp.filePath("Deleted/"))); QVERIFY(QFileInfo::exists(trashed.path + "/sub/weight.bin"));
        auto restored = ops.perform(FileOperations::Action::Copy, trashed.path, temp.filePath("Models")); QVERIFY(restored);
        QVERIFY(ops.perform(FileOperations::Action::Remove, trashed.path)); QVERIFY(!QFileInfo::exists(trashed.path));
        QVERIFY(QFileInfo::exists(restored.path + "/sub/weight.bin"));
    }
    void protectsLayoutAndRejectsEscapes() {
        QTemporaryDir temp(QString(SOCIETY_TEST_DIRECTORY) + "/file-action-boundary-XXXXXX"); QVERIFY(temp.isValid());
        QTemporaryDir outside(QString(SOCIETY_TEST_DIRECTORY) + "/file-action-outside-XXXXXX"); QVERIFY(outside.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive); FileOperations ops(*drive);
        for (const auto &path : {temp.path(), temp.filePath("Models"), temp.filePath("Files/Documents"), temp.filePath("Files/Audios"), temp.filePath("Files/3D objects")}) {
            QVERIFY(!ops.editable(path)); QVERIFY(!ops.perform(FileOperations::Action::Trash, path));
        }
        QFile f(temp.filePath("Files/example.txt")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("keep"); f.close();
        for (const auto &name : {"../escape", ".society-hidden", "a/b", "", "."})
            QVERIFY(!ops.perform(FileOperations::Action::Rename, f.fileName(), name));
        QVERIFY(!ops.perform(FileOperations::Action::Copy, f.fileName(), outside.path()));
        QFile external(outside.filePath("from-finder.txt")); QVERIFY(external.open(QIODevice::WriteOnly)); external.write("external"); external.close();
        const auto pasted = ops.perform(FileOperations::Action::Copy, external.fileName(), temp.filePath("Files/Documents"));
        QVERIFY2(pasted, qPrintable(pasted.error)); QVERIFY(QFileInfo::exists(external.fileName()));
        QVERIFY(!ops.perform(FileOperations::Action::Remove, f.fileName())); // Only Deleted allows permanent removal.
        QVERIFY(QFile::link(outside.path(), temp.filePath("Models/link")));
        QVERIFY(!ops.perform(FileOperations::Action::Trash, temp.filePath("Models/link")));
        QVERIFY(QFile::link(outside.path(), temp.filePath("Deleted/link")));
        QVERIFY(!ops.perform(FileOperations::Action::Copy, f.fileName(), temp.filePath("Deleted/link")));
        QVERIFY(QFileInfo::exists(f.fileName())); QVERIFY(QFileInfo::exists(temp.filePath("Files/Documents")));
    }
};
QTEST_GUILESS_MAIN(FileOperationsTests)
#include "file_operations.moc"
