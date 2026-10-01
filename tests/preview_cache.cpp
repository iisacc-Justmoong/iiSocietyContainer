#include <iiSocietyContainer/PreviewCache.h>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <future>
#include <StorageDirectoryModel.h>
#include <QSignalSpy>
#include <QElapsedTimer>
using namespace iiSocietyContainer;
class PreviewCacheTests : public QObject {
    Q_OBJECT
private slots:
    void persistentHitAndInvalidation() {
        QTemporaryDir root(SOCIETY_TEST_DIRECTORY "/preview-XXXXXX");
        const auto source = root.filePath("source.png"), cache = root.filePath("cache");
        QImage image(2400, 1600, QImage::Format_RGB32); image.fill(Qt::red); QVERIFY(image.save(source));
        PreviewCache first(cache);
        QElapsedTimer timer; timer.start();
        const auto a = first.load(source);
        const auto cold = timer.nsecsElapsed();
        QVERIFY(!a.image.isNull()); QVERIFY(!a.cacheHit); QCOMPARE(a.image.width(), 512);
        QFile input(source); QVERIFY(input.open(QIODevice::ReadOnly));
        QCOMPARE(a.contentHash, QCryptographicHash::hash(input.readAll(), QCryptographicHash::Sha256).toHex()); input.close();
        PreviewCache restarted(cache);
        timer.restart(); const auto b = restarted.load(source);
        qInfo("Preview cold %.3f ms, persisted cache %.3f ms", cold / 1e6, timer.nsecsElapsed() / 1e6);
        QVERIFY(b.cacheHit); QCOMPARE(a.contentHash, b.contentHash); QCOMPARE(a.image, b.image);
        image.fill(Qt::blue); QVERIFY(image.save(source));
        const auto c = restarted.load(source);
        QVERIFY(!c.cacheHit); QVERIFY(c.contentHash != a.contentHash); QCOMPARE(c.image.pixelColor(10, 10), QColor(Qt::blue));
        QVERIFY(QFile::remove(source)); QVERIFY(restarted.load(source).image.isNull());
    }
    void asyncProviderRejectsAnOldRevisionAndSupportsCancellation() {
        QTemporaryDir root(SOCIETY_TEST_DIRECTORY "/preview-XXXXXX");
        const auto source = root.filePath("source.png");
        QImage image(1024, 768, QImage::Format_RGB32); image.fill(Qt::red); QVERIFY(image.save(source));
        const auto url = StorageDirectoryModel::thumbnailUrl(source);
        PreviewProvider provider(root.filePath("cache"));
        auto *response = provider.requestImageResponse(url.path().mid(1), QSize(128, 128));
        QSignalSpy complete(response, &QQuickImageResponse::finished);
        QTRY_COMPARE(complete.size(), 1);
        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        QVERIFY(texture); QCOMPARE(texture->textureSize(), QSize(128, 96)); delete response;
        image.fill(Qt::blue); QVERIFY(image.save(source));
        response = provider.requestImageResponse(url.path().mid(1), QSize(128, 128));
        QSignalSpy stale(response, &QQuickImageResponse::finished);
        response->cancel(); QTRY_COMPARE(stale.size(), 1);
        QVERIFY(!response->textureFactory()); delete response;
    }
    void concurrentRequestsAndCorruptCacheRecover() {
        QTemporaryDir root(SOCIETY_TEST_DIRECTORY "/preview-XXXXXX");
        const auto source = root.filePath("source.png"), cache = root.filePath("cache");
        QImage image(800, 600, QImage::Format_RGB32); image.fill(Qt::green); QVERIFY(image.save(source));
        PreviewCache store(cache);
        auto one = std::async(std::launch::async, [&] { return store.load(source); });
        auto two = std::async(std::launch::async, [&] { return store.load(source); });
        const auto a = one.get(), b = two.get(); QVERIFY(!a.image.isNull()); QCOMPARE(a.contentHash, b.contentHash);
        QCOMPARE(int(a.cacheHit) + int(b.cacheHit), 1);
        QFile corrupt(a.previewPath); QVERIFY(corrupt.open(QIODevice::WriteOnly)); corrupt.write("broken"); corrupt.close();
        QVERIFY(!store.load(source).image.isNull());
        QVERIFY(store.workerCount() >= 1);
    }
};
QTEST_MAIN(PreviewCacheTests)
#include "preview_cache.moc"
