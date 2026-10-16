#include <iiSocietyContainer/PreviewCache.h>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <future>
#include <memory>
#include <vector>
#include <StorageDirectoryModel.h>
#include <SocietyDrive.h>
#include <SharedStorage.h>
#include <QSignalSpy>
#include <QElapsedTimer>
using namespace iiSocietyContainer;
class PreviewCacheTests : public QObject {
    Q_OBJECT
private slots:
    void embeddingPreservesTheThumbnailRequestKey() {
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/preview-key-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto path = fixture.filePath("Files/key.png");
        QImage image(96, 48, QImage::Format_RGB32); image.fill(Qt::blue); QVERIFY(image.save(path));
        const auto before = StorageDirectoryModel::thumbnailUrl(path); QVERIFY(!before.isEmpty());
        PreviewCache cache(fixture.filePath(".society-runtime/cache/key-test"));
        const auto prepared = cache.load(path); QVERIFY(!prepared.image.isNull());
        QCOMPARE(StorageDirectoryModel::thumbnailUrl(path), before);
        image.fill(Qt::red); QVERIFY(image.save(path));
        QVERIFY(StorageDirectoryModel::thumbnailUrl(path) != before);
    }
    void externalAndRedirectedCachesAreNeverCreated() {
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/preview-boundary-XXXXXX");
        const auto drive = SocietyDrive::createAt(fixture.path()); QVERIFY(drive);
        const auto source = drive->sectionPath(StoreSection::Files) + "/source.png";
        QImage image(32, 32, QImage::Format_RGB32); image.fill(Qt::blue); QVERIFY(image.save(source));
        const auto outside = fixture.filePath("outside/cache");
        PreviewCache external(outside); QVERIFY(external.load(source).image.isNull());
        QVERIFY(!QFileInfo::exists(fixture.filePath("outside")));
        const auto previous = qgetenv("IISOCIETY_FILE_CACHE_DIRECTORY");
        qputenv("IISOCIETY_FILE_CACHE_DIRECTORY", outside.toUtf8());
        PreviewCache defaultCache; QVERIFY(defaultCache.load(source).image.isNull());
        QVERIFY(!QFileInfo::exists(fixture.filePath("outside")));
        qputenv("IISOCIETY_FILE_CACHE_DIRECTORY", previous);
        const auto internal = defaultCache.load(source);
        QVERIFY(!internal.image.isNull());
        QVERIFY(internal.previewPath.startsWith(drive->rootPath() + "/.society-runtime/cache/files/"));
    }
    void persistentHitAndInvalidation() {
        QTemporaryDir root(SOCIETY_TEST_DIRECTORY "/preview-XXXXXX.societycontainer");
        QVERIFY(SocietyDrive::create(root.path()));
        const auto source = root.filePath("Files/source.png"), cache = root.filePath(".society-runtime/cache/test-previews");
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
        QTemporaryDir root(SOCIETY_TEST_DIRECTORY "/preview-XXXXXX.societycontainer");
        QVERIFY(SocietyDrive::create(root.path()));
        const auto source = root.filePath("Files/source.png");
        QImage image(1024, 768, QImage::Format_RGB32); image.fill(Qt::red); QVERIFY(image.save(source));
        const auto url = StorageDirectoryModel::thumbnailUrl(source);
        PreviewProvider provider(root.filePath(".society-runtime/cache/test-previews"));
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
        // Queue more jobs than workers, then allow application shutdown to
        // join every worker after cancellation and normal completions.
        std::vector<std::unique_ptr<QQuickImageResponse>> requests;
        std::vector<std::unique_ptr<QSignalSpy>> completions;
        const auto current = StorageDirectoryModel::thumbnailUrl(source).path().mid(1);
        for (unsigned i = 0; i < 2 * PreviewCache::workerCount(); ++i) {
            requests.emplace_back(provider.requestImageResponse(current, QSize(64, 64)));
            completions.push_back(std::make_unique<QSignalSpy>(requests.back().get(), &QQuickImageResponse::finished));
        }
        for (unsigned i = 0; i < requests.size(); ++i) {
            QTRY_COMPARE(completions[i]->size(), 1);
            std::unique_ptr<QQuickTextureFactory> completed(requests[i]->textureFactory());
            QVERIFY(completed); QCOMPARE(completed->textureSize(), QSize(64, 48));
        }
    }
    void concurrentRequestsAndCorruptCacheRecover() {
        QTemporaryDir root(SOCIETY_TEST_DIRECTORY "/preview-XXXXXX.societycontainer");
        QVERIFY(SocietyDrive::create(root.path()));
        const auto source = root.filePath("Files/source.png"), cache = root.filePath(".society-runtime/cache/test-previews");
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
