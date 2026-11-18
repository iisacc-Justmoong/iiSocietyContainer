#include <iiSocietyContainer/PreviewCache.h>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include "QtEventWait.h"
#include "SignalRecorder.h"
#include "PreviewResponseGuard.h"
#include <QGuiApplication>
#include <QEvent>
#include <future>
#include <atomic>
#include <memory>
#include <vector>
#include <StorageDirectoryModel.h>
#include <SocietyDrive.h>
#include <SharedStorage.h>
#include <QElapsedTimer>
#include <QBuffer>
#include <QScopeGuard>
#include <iiFileProviderPresentation.h>
using namespace iiSocietyContainer;
class PreviewCacheTests : public QObject {
public:
    void nativeInformationCardDoesNotHideAnExistingLargeImageDecoder()
    {
        QTemporaryDir root(QDir::currentPath() + "/large-image-XXXXXX.societycontainer");
        CTEST_CHECK(SocietyDrive::create(root.path()));
        const auto source = root.filePath("Files/large.png");
        QImage image(80, 40, QImage::Format_RGB32); image.fill(Qt::blue); CTEST_CHECK(image.save(source));
        // PNG decoders accept padding after IEND. Exceed the native provider's
        // 32 MiB budget without allocating a large test raster.
        QFile file(source); CTEST_CHECK(file.open(QIODevice::ReadWrite));
        CTEST_CHECK(file.resize(33ll * 1024 * 1024)); file.close();
        PreviewCache cache;
        const auto first = cache.load(source, PreviewCache::HashPolicy::PreviewOnly);
        CTEST_CHECK(!first.image.isNull());
        CTEST_COMPARE(first.image.pixelColor(10, 10), QColor(Qt::blue));
        CTEST_CHECK(first.contentHash.isEmpty());
        const auto next = cache.load(source, PreviewCache::HashPolicy::PreviewOnly);
        CTEST_CHECK(next.cacheHit); CTEST_COMPARE(next.image, first.image);
    }
    void ordinaryFolderImagesHaveReadOnlyPreviewsWithoutACache()
    {
        QTemporaryDir root(QDir::currentPath() + "/external-image-XXXXXX");
        const auto source = root.filePath("image.png");
        QImage image(80, 40, QImage::Format_RGB32); image.fill(Qt::cyan); CTEST_CHECK(image.save(source));
        QFile file(source); CTEST_CHECK(file.open(QIODevice::ReadOnly)); const auto original = file.readAll(); file.close();
        PreviewCache cache;
        const auto preview = cache.load(source, PreviewCache::HashPolicy::PreviewOnly);
        CTEST_CHECK(!preview.image.isNull()); CTEST_CHECK(preview.contentHash.isEmpty());
        CTEST_CHECK(preview.previewPath.isEmpty()); CTEST_CHECK(!preview.cacheHit);
        CTEST_COMPARE(preview.image.pixelColor(10, 10), QColor(Qt::cyan));
        CTEST_CHECK(cache.load(source).image.isNull());
        CTEST_COMPARE(QDir(root.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{"image.png"});
        CTEST_CHECK(file.open(QIODevice::ReadOnly)); CTEST_COMPARE(file.readAll(), original);
    }
    void previewOnlyUsesProviderMediaWithoutHashingTheOriginal() {
        QTemporaryDir root(QDir::currentPath() + "/preview-only-XXXXXX.societycontainer");
        CTEST_CHECK(SocietyDrive::create(root.path()));
        const auto source = root.filePath("Files/study.iisc");
        { QFile file(source); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("document fixture");
          CTEST_CHECK(file.resize(16ll * 1024 * 1024)); }
        QImage image(120, 80, QImage::Format_RGB32); image.fill(Qt::yellow);
        QByteArray png; QBuffer buffer(&png); CTEST_CHECK(buffer.open(QIODevice::WriteOnly));
        CTEST_CHECK(image.save(&buffer, "PNG"));
        std::atomic_int renders = 0;
        using iiFileProvider::native::Presentation;
        Presentation::setRenderer(".iisc", [&](const auto &, auto &) -> std::optional<iiFileProvider::native::FilePreview> {
            ++renders;
            return iiFileProvider::native::FilePreview{"image/png", "image", png.toStdString(), 120, 80};
        });
        const auto cleanup = qScopeGuard([] { Presentation::setRenderer(".iisc", {}); });
        const auto url = StorageDirectoryModel::thumbnailUrl(source); CTEST_CHECK(!url.isEmpty());
        PreviewCache cache(root.filePath(".society-runtime/cache/preview-only"));
        QElapsedTimer timer; timer.start();
        const auto first = cache.load(source, PreviewCache::HashPolicy::PreviewOnly);
        const auto firstMs = timer.nsecsElapsed() / 1e6;
        CTEST_CHECK(!first.image.isNull()); CTEST_CHECK(first.contentHash.isEmpty()); CTEST_CHECK(!first.cacheHit);
        CTEST_COMPARE(first.image.pixelColor(10, 10), QColor(Qt::yellow));
        const auto rendered = renders.load();
        timer.restart();
        const auto second = cache.load(source, PreviewCache::HashPolicy::PreviewOnly);
        const auto persistedMs = timer.nsecsElapsed() / 1e6;
        CTEST_CHECK(second.cacheHit); CTEST_COMPARE(renders.load(), rendered);
        CTEST_COMPARE(second.image, first.image);
        timer.restart(); const auto verified = cache.load(source);
        const auto verifiedMs = timer.nsecsElapsed() / 1e6;
        CTEST_CHECK(!verified.contentHash.isEmpty());
        QFile original(source); CTEST_CHECK(original.open(QIODevice::ReadOnly));
        QCryptographicHash expected(QCryptographicHash::Sha256); CTEST_CHECK(expected.addData(&original)); original.close();
        CTEST_COMPARE(verified.contentHash, expected.result().toHex());
        qInfo("16 MiB native preview first %.3f ms, persisted %.3f ms, explicit whole-source hash %.3f ms",
              firstMs, persistedMs, verifiedMs);
        CTEST_COMPARE(renders.load(), rendered);
        QFile changed(source); CTEST_CHECK(changed.open(QIODevice::WriteOnly)); changed.write("changed document"); changed.close();
        const auto next = cache.load(source, PreviewCache::HashPolicy::PreviewOnly);
        CTEST_CHECK(!next.image.isNull()); CTEST_CHECK(!next.cacheHit);
        CTEST_CHECK(renders.load() > rendered);
    }
    void embeddingPreservesTheThumbnailRequestKey() {
        QTemporaryDir fixture(QDir::currentPath() + "/preview-key-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto path = fixture.filePath("Files/key.png");
        QImage image(96, 48, QImage::Format_RGB32); image.fill(Qt::blue); CTEST_CHECK(image.save(path));
        const auto before = StorageDirectoryModel::thumbnailUrl(path); CTEST_CHECK(!before.isEmpty());
        PreviewCache cache(fixture.filePath(".society-runtime/cache/key-test"));
        const auto prepared = cache.load(path); CTEST_CHECK(!prepared.image.isNull());
        CTEST_COMPARE(StorageDirectoryModel::thumbnailUrl(path), before);
        image.fill(Qt::red); CTEST_CHECK(image.save(path));
        CTEST_CHECK(StorageDirectoryModel::thumbnailUrl(path) != before);
    }
    void externalAndRedirectedCachesAreNeverCreated() {
        QTemporaryDir fixture(QDir::currentPath() + "/preview-boundary-XXXXXX");
        const auto drive = SocietyDrive::createAt(fixture.path()); CTEST_CHECK(drive);
        const auto source = drive->sectionPath(StoreSection::Files) + "/source.png";
        QImage image(32, 32, QImage::Format_RGB32); image.fill(Qt::blue); CTEST_CHECK(image.save(source));
        const auto outside = fixture.filePath("outside/cache");
        PreviewCache external(outside); CTEST_CHECK(external.load(source).image.isNull());
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("outside")));
        const auto previous = qgetenv("IISOCIETY_FILE_CACHE_DIRECTORY");
        const auto restoreEnvironment = [&] {
            if (previous.isNull()) qunsetenv("IISOCIETY_FILE_CACHE_DIRECTORY");
            else qputenv("IISOCIETY_FILE_CACHE_DIRECTORY", previous);
        };
        const auto restoreCacheDirectory = qScopeGuard(restoreEnvironment);
        qputenv("IISOCIETY_FILE_CACHE_DIRECTORY", outside.toUtf8());
        PreviewCache defaultCache; CTEST_CHECK(defaultCache.load(source).image.isNull());
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("outside")));
        restoreEnvironment();
        const auto internal = defaultCache.load(source);
        CTEST_CHECK(!internal.image.isNull());
        CTEST_CHECK(internal.previewPath.startsWith(drive->rootPath() + "/.society-runtime/cache/files/"));
    }
    void persistentHitAndInvalidation() {
        QTemporaryDir root(QDir::currentPath() + "/preview-XXXXXX.societycontainer");
        CTEST_CHECK(SocietyDrive::create(root.path()));
        const auto source = root.filePath("Files/source.png"), cache = root.filePath(".society-runtime/cache/test-previews");
        QImage image(2400, 1600, QImage::Format_RGB32); image.fill(Qt::red); CTEST_CHECK(image.save(source));
        PreviewCache first(cache);
        QElapsedTimer timer; timer.start();
        const auto a = first.load(source);
        const auto cold = timer.nsecsElapsed();
        CTEST_CHECK(!a.image.isNull()); CTEST_CHECK(!a.cacheHit); CTEST_COMPARE(a.image.width(), 512);
        QFile input(source); CTEST_CHECK(input.open(QIODevice::ReadOnly));
        CTEST_COMPARE(a.contentHash, QCryptographicHash::hash(input.readAll(), QCryptographicHash::Sha256).toHex()); input.close();
        PreviewCache restarted(cache);
        timer.restart(); const auto b = restarted.load(source);
        qInfo("Preview cold %.3f ms, persisted cache %.3f ms", cold / 1e6, timer.nsecsElapsed() / 1e6);
        CTEST_CHECK(b.cacheHit); CTEST_COMPARE(a.contentHash, b.contentHash); CTEST_COMPARE(a.image, b.image);
        image.fill(Qt::blue); CTEST_CHECK(image.save(source));
        const auto c = restarted.load(source);
        CTEST_CHECK(!c.cacheHit); CTEST_CHECK(c.contentHash != a.contentHash); CTEST_COMPARE(c.image.pixelColor(10, 10), QColor(Qt::blue));
        CTEST_CHECK(QFile::remove(source)); CTEST_CHECK(restarted.load(source).image.isNull());
    }
    void asyncProviderRejectsAnOldRevisionAndSupportsCancellation() {
        QTemporaryDir root(QDir::currentPath() + "/preview-XXXXXX.societycontainer");
        CTEST_CHECK(SocietyDrive::create(root.path()));
        const auto source = root.filePath("Files/source.png");
        QImage image(1024, 768, QImage::Format_RGB32); image.fill(Qt::red); CTEST_CHECK(image.save(source));
        const auto url = StorageDirectoryModel::thumbnailUrl(source);
        PreviewProvider provider(root.filePath(".society-runtime/cache/test-previews"));
        using FinishedRecorder = society_ctest::SignalRecorder<decltype(&QQuickImageResponse::finished)>;
        {
            std::unique_ptr<FinishedRecorder> complete;
            std::unique_ptr<QQuickImageResponse> response(provider.requestImageResponse(url.path().mid(1), QSize(128, 128)));
            const auto retire = qScopeGuard([&] { society_ctest::retirePreviewResponse(response, complete.get()); });
            CTEST_CHECK(response);
            complete = std::make_unique<FinishedRecorder>(response.get(), &QQuickImageResponse::finished);
            CTEST_WAIT_COMPARE(complete->size(), 1);
            std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
            CTEST_CHECK(texture); CTEST_COMPARE(texture->textureSize(), QSize(128, 96));
        }
        image.fill(Qt::blue); CTEST_CHECK(image.save(source));
        {
            std::unique_ptr<FinishedRecorder> stale;
            std::unique_ptr<QQuickImageResponse> response(provider.requestImageResponse(url.path().mid(1), QSize(128, 128)));
            const auto retire = qScopeGuard([&] { society_ctest::retirePreviewResponse(response, stale.get()); });
            CTEST_CHECK(response);
            stale = std::make_unique<FinishedRecorder>(response.get(), &QQuickImageResponse::finished);
            response->cancel(); CTEST_WAIT_COMPARE(stale->size(), 1);
            std::unique_ptr<QQuickTextureFactory> cancelled(response->textureFactory());
            CTEST_CHECK(!cancelled);
        }
        // Queue more jobs than workers, then allow application shutdown to
        // join every worker after cancellation and normal completions.
        std::vector<std::unique_ptr<QQuickImageResponse>> requests;
        std::vector<std::unique_ptr<FinishedRecorder>> completions;
        const auto retireRequests = qScopeGuard([&] {
            for (std::size_t i = 0; i < requests.size(); ++i)
                society_ctest::retirePreviewResponse(requests[i], i < completions.size() ? completions[i].get() : nullptr);
        });
        const auto current = StorageDirectoryModel::thumbnailUrl(source).path().mid(1);
        for (unsigned i = 0; i < 2 * PreviewCache::workerCount(); ++i) {
            requests.emplace_back(provider.requestImageResponse(current, QSize(64, 64)));
            CTEST_CHECK(requests.back());
            completions.push_back(std::make_unique<FinishedRecorder>(requests.back().get(), &QQuickImageResponse::finished));
        }
        for (unsigned i = 0; i < requests.size(); ++i) {
            CTEST_WAIT_COMPARE(completions[i]->size(), 1);
            std::unique_ptr<QQuickTextureFactory> completed(requests[i]->textureFactory());
            CTEST_CHECK(completed); CTEST_COMPARE(completed->textureSize(), QSize(64, 48));
        }
    }
    void concurrentRequestsAndCorruptCacheRecover() {
        QTemporaryDir root(QDir::currentPath() + "/preview-XXXXXX.societycontainer");
        CTEST_CHECK(SocietyDrive::create(root.path()));
        const auto source = root.filePath("Files/source.png"), cache = root.filePath(".society-runtime/cache/test-previews");
        QImage image(800, 600, QImage::Format_RGB32); image.fill(Qt::green); CTEST_CHECK(image.save(source));
        PreviewCache store(cache);
        auto one = std::async(std::launch::async, [&] { return store.load(source); });
        auto two = std::async(std::launch::async, [&] { return store.load(source); });
        const auto a = one.get(), b = two.get(); CTEST_CHECK(!a.image.isNull()); CTEST_COMPARE(a.contentHash, b.contentHash);
        CTEST_COMPARE(int(a.cacheHit) + int(b.cacheHit), 1);
        QImage replacement(128, 96, QImage::Format_RGB32); replacement.fill(Qt::blue);
        CTEST_CHECK(replacement.save(a.previewPath)); // Valid PNG, wrong digest.
        const auto repaired = store.load(source);
        CTEST_CHECK(!repaired.cacheHit);
        CTEST_COMPARE(repaired.image.pixelColor(10, 10), QColor(Qt::green));
        QFile corrupt(a.previewPath); CTEST_CHECK(corrupt.open(QIODevice::WriteOnly)); corrupt.write("broken"); corrupt.close();
        CTEST_CHECK(!store.load(source).image.isNull());
        CTEST_CHECK(store.workerCount() >= 1);
    }
};
int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    society_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    PreviewCacheTests tests;
    runner.run("nativeInformationCardDoesNotHideAnExistingLargeImageDecoder", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.nativeInformationCardDoesNotHideAnExistingLargeImageDecoder(); });
    runner.run("ordinaryFolderImagesHaveReadOnlyPreviewsWithoutACache", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.ordinaryFolderImagesHaveReadOnlyPreviewsWithoutACache(); });
    runner.run("previewOnlyUsesProviderMediaWithoutHashingTheOriginal", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.previewOnlyUsesProviderMediaWithoutHashingTheOriginal(); });
    runner.run("embeddingPreservesTheThumbnailRequestKey", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.embeddingPreservesTheThumbnailRequestKey(); });
    runner.run("externalAndRedirectedCachesAreNeverCreated", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.externalAndRedirectedCachesAreNeverCreated(); });
    runner.run("persistentHitAndInvalidation", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.persistentHitAndInvalidation(); });
    runner.run("asyncProviderRejectsAnOldRevisionAndSupportsCancellation", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.asyncProviderRejectsAnOldRevisionAndSupportsCancellation(); });
    runner.run("concurrentRequestsAndCorruptCacheRecover", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.concurrentRequestsAndCorruptCacheRecover(); });
    return runner.result();
}
