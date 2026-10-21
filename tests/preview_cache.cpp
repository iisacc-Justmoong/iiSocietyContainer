#include <iiSocietyContainer/PreviewCache.h>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <future>
#include <atomic>
#include <memory>
#include <vector>
#include <StorageDirectoryModel.h>
#include <SocietyDrive.h>
#include <SharedStorage.h>
#include <QSignalSpy>
#include <QElapsedTimer>
#include <QBuffer>
#include <QScopeGuard>
#include <iiFileProviderPresentation.h>
using namespace iiSocietyContainer;
class PreviewCacheTests : public QObject {
    Q_OBJECT
private slots:
    void nativeInformationCardDoesNotHideAnExistingLargeImageDecoder()
    {
        QTemporaryDir root(SOCIETY_TEST_DIRECTORY "/large-image-XXXXXX.societycontainer");
        QVERIFY(SocietyDrive::create(root.path()));
        const auto source = root.filePath("Files/large.png");
        QImage image(80, 40, QImage::Format_RGB32); image.fill(Qt::blue); QVERIFY(image.save(source));
        // PNG decoders accept padding after IEND. Exceed the native provider's
        // 32 MiB budget without allocating a large test raster.
        QFile file(source); QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(33ll * 1024 * 1024)); file.close();
        PreviewCache cache;
        const auto first = cache.load(source, PreviewCache::HashPolicy::PreviewOnly);
        QVERIFY(!first.image.isNull());
        QCOMPARE(first.image.pixelColor(10, 10), QColor(Qt::blue));
        QVERIFY(first.contentHash.isEmpty());
        const auto next = cache.load(source, PreviewCache::HashPolicy::PreviewOnly);
        QVERIFY(next.cacheHit); QCOMPARE(next.image, first.image);
    }
    void ordinaryFolderImagesHaveReadOnlyPreviewsWithoutACache()
    {
        QTemporaryDir root(SOCIETY_TEST_DIRECTORY "/external-image-XXXXXX");
        const auto source = root.filePath("image.png");
        QImage image(80, 40, QImage::Format_RGB32); image.fill(Qt::cyan); QVERIFY(image.save(source));
        QFile file(source); QVERIFY(file.open(QIODevice::ReadOnly)); const auto original = file.readAll(); file.close();
        PreviewCache cache;
        const auto preview = cache.load(source, PreviewCache::HashPolicy::PreviewOnly);
        QVERIFY(!preview.image.isNull()); QVERIFY(preview.contentHash.isEmpty());
        QVERIFY(preview.previewPath.isEmpty()); QVERIFY(!preview.cacheHit);
        QCOMPARE(preview.image.pixelColor(10, 10), QColor(Qt::cyan));
        QVERIFY(cache.load(source).image.isNull());
        QCOMPARE(QDir(root.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{"image.png"});
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original);
    }
    void previewOnlyUsesProviderMediaWithoutHashingTheOriginal() {
        QTemporaryDir root(SOCIETY_TEST_DIRECTORY "/preview-only-XXXXXX.societycontainer");
        QVERIFY(SocietyDrive::create(root.path()));
        const auto source = root.filePath("Files/study.iisc");
        { QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("document fixture");
          QVERIFY(file.resize(16ll * 1024 * 1024)); }
        QImage image(120, 80, QImage::Format_RGB32); image.fill(Qt::yellow);
        QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
        std::atomic_int renders = 0;
        using iiFileProvider::native::Presentation;
        Presentation::setRenderer(".iisc", [&](const auto &, auto &) -> std::optional<iiFileProvider::native::FilePreview> {
            ++renders;
            return iiFileProvider::native::FilePreview{"image/png", "image", png.toStdString(), 120, 80};
        });
        const auto cleanup = qScopeGuard([] { Presentation::setRenderer(".iisc", {}); });
        const auto url = StorageDirectoryModel::thumbnailUrl(source); QVERIFY(!url.isEmpty());
        PreviewCache cache(root.filePath(".society-runtime/cache/preview-only"));
        QElapsedTimer timer; timer.start();
        const auto first = cache.load(source, PreviewCache::HashPolicy::PreviewOnly);
        const auto firstMs = timer.nsecsElapsed() / 1e6;
        QVERIFY(!first.image.isNull()); QVERIFY(first.contentHash.isEmpty()); QVERIFY(!first.cacheHit);
        QCOMPARE(first.image.pixelColor(10, 10), QColor(Qt::yellow));
        const auto rendered = renders.load();
        timer.restart();
        const auto second = cache.load(source, PreviewCache::HashPolicy::PreviewOnly);
        const auto persistedMs = timer.nsecsElapsed() / 1e6;
        QVERIFY(second.cacheHit); QCOMPARE(renders.load(), rendered);
        QCOMPARE(second.image, first.image);
        timer.restart(); const auto verified = cache.load(source);
        const auto verifiedMs = timer.nsecsElapsed() / 1e6;
        QVERIFY(!verified.contentHash.isEmpty());
        QFile original(source); QVERIFY(original.open(QIODevice::ReadOnly));
        QCryptographicHash expected(QCryptographicHash::Sha256); QVERIFY(expected.addData(&original)); original.close();
        QCOMPARE(verified.contentHash, expected.result().toHex());
        qInfo("16 MiB native preview first %.3f ms, persisted %.3f ms, explicit whole-source hash %.3f ms",
              firstMs, persistedMs, verifiedMs);
        QCOMPARE(renders.load(), rendered);
        QFile changed(source); QVERIFY(changed.open(QIODevice::WriteOnly)); changed.write("changed document"); changed.close();
        const auto next = cache.load(source, PreviewCache::HashPolicy::PreviewOnly);
        QVERIFY(!next.image.isNull()); QVERIFY(!next.cacheHit);
        QVERIFY(renders.load() > rendered);
    }
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
        QImage replacement(128, 96, QImage::Format_RGB32); replacement.fill(Qt::blue);
        QVERIFY(replacement.save(a.previewPath)); // Valid PNG, wrong digest.
        const auto repaired = store.load(source);
        QVERIFY(!repaired.cacheHit);
        QCOMPARE(repaired.image.pixelColor(10, 10), QColor(Qt::green));
        QFile corrupt(a.previewPath); QVERIFY(corrupt.open(QIODevice::WriteOnly)); corrupt.write("broken"); corrupt.close();
        QVERIFY(!store.load(source).image.isNull());
        QVERIFY(store.workerCount() >= 1);
    }
};
QTEST_MAIN(PreviewCacheTests)
#include "preview_cache.moc"
