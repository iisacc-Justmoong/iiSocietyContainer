#include <iiSocietyContainer/DashboardFiles.h>
#include <iiSocietyContainer/SocietyApplication.h>
#include <SocietyDrive.h>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileOpenEvent>
#include <QImage>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace iiSocietyContainer;

class DashboardTests : public QObject {
    Q_OBJECT
private slots:
    void historyIsPersistedBoundedAndObserved() {
        QTemporaryDir root(SOCIETY_TEST_DIRECTORY "/history-XXXXXX");
        QVERIFY(SocietyDrive::create(root.path()));
        QImage image(16, 16, QImage::Format_RGB32); image.fill(Qt::cyan);
        for (int i = 0; i < 25; ++i) {
            const auto path = root.filePath(QString("Generation History/image-%1.png").arg(i, 2, 10, QChar('0')));
            QVERIFY(image.save(path));
            QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-60-i), QFileDevice::FileModificationTime));
        }
        QVERIFY(image.save(root.filePath("Files/separate.png")));
        for (int i = 0; i < 6; ++i) {
            const auto path = root.filePath(QString("Published/published-%1.png").arg(i, 2, 10, QChar('0')));
            QVERIFY(image.save(path));
            QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-30-i), QFileDevice::FileModificationTime));
        }
        QVERIFY(image.save(root.filePath("Generation History/.hidden.png")));
        QVERIFY(QDir().mkpath(root.filePath("Generation History/legacy")));
        QVERIFY(image.save(root.filePath("Generation History/legacy/nested.png")));
        QVERIFY(QFile::link(root.filePath("Files/separate.png"), root.filePath("Generation History/link.png")));
        QFile metadata(root.filePath("Generation History/request.json"));
        QVERIFY(metadata.open(QIODevice::WriteOnly)); metadata.write("{}"); metadata.close();
        DashboardFiles model; model.setContainerPath(root.path());
        QTRY_VERIFY(!model.loading());
        QCOMPARE(model.generationHistory().size(), 20);
        QCOMPARE(model.generationHistory().first().toMap().value("name").toString(), "image-00.png");
        QCOMPARE(model.generationHistory().last().toMap().value("name").toString(), "image-19.png");
        QCOMPARE(model.recentFiles().size(), 1);
        QCOMPARE(model.recentPublished().size(), 4);
        QCOMPARE(model.recentPublished().first().toMap().value("name").toString(), "published-00.png");
        QCOMPARE(model.recentPublished().last().toMap().value("name").toString(), "published-03.png");
        QSignalSpy changed(&model, &DashboardFiles::filesChanged);
        model.refresh(); QTRY_VERIFY(!model.loading());
        QCOMPARE(changed.size(), 0);
        QVERIFY(image.save(root.filePath("Generation History/latest.png")));
        QTRY_COMPARE(model.generationHistory().first().toMap().value("name").toString(), "latest.png");
        const auto preview = model.generationHistory().first().toMap().value("previewSource");
        QSaveFile replacement(root.filePath("Generation History/latest.png"));
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        QImage larger(64, 64, QImage::Format_RGB32); larger.fill(Qt::yellow);
        QVERIFY(larger.save(&replacement, "PNG")); QVERIFY(replacement.commit());
        QTRY_VERIFY(model.generationHistory().first().toMap().value("previewSource") != preview);
        QVERIFY(QFile::remove(root.filePath("Generation History/latest.png")));
        QTRY_COMPARE(model.generationHistory().first().toMap().value("name").toString(), "image-00.png");
        model.setQuery("image-24");
        QCOMPARE(model.generationHistory().size(), 1);
        QCOMPARE(model.generationHistory().first().toMap().value("name").toString(), "image-24.png");
        model.refresh(); model.setContainerPath("");
        QTRY_VERIFY(!model.loading()); QVERIFY(model.generationHistory().isEmpty());
        model.setContainerPath("relative");
        QVERIFY(model.generationHistory().isEmpty()); QVERIFY(!model.errorString().isEmpty());
    }
    void onlyHistoryLinksAreAccepted() {
        SocietyApplication application;
        QSignalSpy requested(&application, &SocietyApplication::generationHistoryRequested);
        for (const auto &url : {"https://generation-history", "society://models", "society://generation-history/../Models",
                               "society://generation-history?path=/tmp", "society://user@generation-history", "society://generation-history:123"})
            QVERIFY(!application.handleUrl(QUrl(url)));
        QCOMPARE(requested.size(), 0);
        QVERIFY(application.handleUrl(QUrl("society://generation-history")));
        QCOMPARE(requested.size(), 1);
        application.listen();
        QFileOpenEvent event(QUrl("society://generation-history"));
        QCoreApplication::sendEvent(QCoreApplication::instance(), &event);
        QCOMPARE(requested.size(), 2);
        QVERIFY(application.openGenerationHistory());
        QCOMPARE(requested.size(), 3);
    }
};
QTEST_MAIN(DashboardTests)
#include "dashboard.moc"
