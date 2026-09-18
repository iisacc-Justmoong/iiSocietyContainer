#include "FileActions.h"
#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QMimeData>
#include <QLockFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
using namespace iiSocietyContainer;
class FileActionsTests : public QObject {
    Q_OBJECT
private slots:
    void deletionNeverQueuesDownloads() {
        QTemporaryDir temp(QString(SOCIETY_TEST_DIRECTORY) + "/delete-no-pull-XXXXXX"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive); StorageMap map(*drive);
        const auto source = temp.filePath("Models/Checkpoint/model.safetensors");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.resize(8LL * 1024 * 1024 * 1024)); file.close();
        QJsonObject entry{{"path", "models/Checkpoint/model.safetensors"}, {"kind", "file"},
            {"size", "1"}, {"resident", false}, {"version", QString(64, 'a')}};
        QVERIFY(map.publish({entry}));
        QLockFile sync(temp.filePath(".society-sync/operation.lock")); QVERIFY(sync.tryLock());
        FileActions actions; QSignalSpy done(&actions, &FileActions::completed), failed(&actions, &FileActions::failed);
        actions.setPath(source); actions.trash();
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 1500);
        QCOMPARE(done.size(), 1); QVERIFY(failed.isEmpty()); QVERIFY(map.pendingRequests().isEmpty());
        const auto deleted = done.first().first().toString(); QVERIFY(QFileInfo::exists(deleted));
        entry["path"] = "deleted/model.safetensors"; QVERIFY(map.publish({entry}));
        actions.setPath(deleted); actions.remove();
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 1500);
        QCOMPARE(done.size(), 2); QVERIFY(failed.isEmpty()); QVERIFY(map.pendingRequests().isEmpty());
        QVERIFY(!QFileInfo::exists(deleted));
        // Remote-only files cannot be removed locally, and must not be fetched to delete them.
        entry["path"] = "models/remote.safetensors"; QVERIFY(map.publish({entry}));
        actions.setPath(temp.filePath("Models/remote.safetensors")); actions.trash();
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 1500);
        QCOMPARE(failed.size(), 1); QCOMPARE(done.size(), 2); QVERIFY(map.pendingRequests().isEmpty());
    }
    void copiesUrlsAndPastesWithoutOverwriting() {
        QTemporaryDir temp(QString(SOCIETY_TEST_DIRECTORY) + "/clipboard-XXXXXX"); QVERIFY(temp.isValid());
        QVERIFY(SocietyDrive::create(temp.path()));
        const auto source = temp.filePath("Models/test.safetensors");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("weights"); file.close();
        FileActions actions; QSignalSpy done(&actions, &FileActions::completed); QSignalSpy failed(&actions, &FileActions::failed);
        actions.setPath(source); actions.copy();
        QTRY_COMPARE(done.size(), 1); QVERIFY(failed.isEmpty());
        QCOMPARE(QGuiApplication::clipboard()->mimeData()->urls(), QList<QUrl>{QUrl::fromLocalFile(source)});
        QVERIFY(actions.canPaste());
        actions.paste(temp.filePath("Models")); QTRY_COMPARE(done.size(), 2); QVERIFY(failed.isEmpty());
        QVERIFY(QFileInfo::exists(temp.filePath("Models/test 2.safetensors"))); QVERIFY(QFileInfo::exists(source));
        actions.rename("updated.safetensors"); QTRY_COMPARE(done.size(), 3); QVERIFY(!QFileInfo::exists(source));
        actions.setPath(temp.filePath("Models/updated.safetensors")); actions.trash(); QTRY_COMPARE(done.size(), 4);
        QVERIFY(QFileInfo::exists(temp.filePath("Deleted/updated.safetensors"))); QVERIFY(failed.isEmpty());
    }
    void metadataMenuDoesNotPullAndCopyWaitsForSelectedObject() {
        QTemporaryDir temp(QString(SOCIETY_TEST_DIRECTORY) + "/remote-actions-XXXXXX"); QVERIFY(temp.isValid());
        const auto drive = SocietyDrive::create(temp.path()); QVERIFY(drive); StorageMap map(*drive);
        QJsonObject entry{{"path", "models/remote.bin"}, {"kind", "file"}, {"size", "3"}, {"version", QString(64, 'a')}, {"resident", false}};
        QVERIFY(map.publish({entry})); FileActions actions; actions.setPath(temp.filePath("Models/remote.bin"));
        QVERIFY(actions.editable()); QVERIFY(map.pendingRequests().isEmpty());
        QSignalSpy done(&actions, &FileActions::completed); actions.copy();
        QTRY_COMPARE(map.pendingRequests().size(), 1); QCOMPARE(done.size(), 0); QVERIFY(actions.busy());
        const auto request = map.pendingRequests().first().toObject();
        QCOMPARE(request.value("objects").toArray().size(), 1);
        QCOMPARE(request.value("objects").toArray().first().toObject().value("path"), entry.value("path"));
        QFile file(actions.path()); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("new"); file.close();
        entry["resident"] = true; QVERIFY(map.publish({entry})); QVERIFY(map.finishRequest(request.value("id").toString()));
        QTRY_COMPARE(done.size(), 1); QVERIFY(!actions.busy());
        QCOMPARE(QGuiApplication::clipboard()->mimeData()->urls().first().toLocalFile(), actions.path());
    }
};
QTEST_MAIN(FileActionsTests)
#include "file_actions.moc"
