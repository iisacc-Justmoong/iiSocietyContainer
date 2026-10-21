#include <iiFileProviderDevice.h>
#include "PreviewCache.h"
#include <iiFileProviderPresentation.h>
#include <StorageDirectoryModel.h>
#include <FileOperations.h>
#include <SharedStorage.h>
#include <QCryptographicHash>
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QQuickTextureFactory>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace iiSocietyContainer {
namespace {
std::array<std::mutex, 64> entryLocks;
std::mutex pruneLock;
constexpr qint64 cacheBudget = 512ll * 1024 * 1024;
void prune(const QString &directory) {
    std::lock_guard lock(pruneLock);
    const auto files = QDir(directory).entryInfoList({"*.png", "*.json"}, QDir::Files | QDir::NoSymLinks, QDir::Time);
    qint64 total = 0;
    for (const auto &file : files) {
        total += file.size();
        if (total > cacheBudget) iiFileProvider::FileDevice::remove(file.absoluteFilePath());
    }
}
// Standard C++ executor, independent of Qt's global pool and sync/hash workers.
class Workers {
public:
    Workers() {
        for (unsigned i = 0; i < PreviewCache::workerCount(); ++i) threads.emplace_back([this] {
            for (;;) {
                std::function<void()> job;
                { std::unique_lock lock(mutex); ready.wait(lock, [this] { return stopping || !queue.empty(); });
                  if (stopping && queue.empty()) return;
                  job = std::move(queue.front()); queue.pop_front(); }
                job();
            }
        });
    }
    ~Workers() { shutdown(); }
    void shutdown() {
        { std::lock_guard lock(mutex); stopping = true; }
        ready.notify_all();
        for (auto &thread : threads)
            if (thread.joinable()) thread.join(); // Join while Qt/CRT DLLs are still loaded.
        threads.clear();
    }
    void submit(std::function<void()> work) {
        { std::lock_guard lock(mutex); queue.push_back(std::move(work)); } ready.notify_one();
    }
private:
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> queue;
    bool stopping = false;
    // Destroy/join before the queue and its synchronization primitives.
    // NDK r27 does not expose std::jthread. The pool already owns its stop
    // condition, so explicit std::thread joining preserves the same lifetime.
    std::vector<std::thread> threads;
};
Workers &workers() {
    static Workers pool;
    static const bool registered = [] {
        qAddPostRoutine([] { pool.shutdown(); });
        return true;
    }();
    (void)registered;
    return pool;
}
class Response final : public QQuickImageResponse {
public:
    Response(std::shared_ptr<PreviewCache> cache, QString id, QSize size) {
        workers().submit([this, cache = std::move(cache), id = std::move(id), size] {
            try {
                const auto path = QString::fromUtf8(QByteArray::fromBase64(id.section('/', 0, 0).toLatin1(), QByteArray::Base64UrlEncoding));
                const auto expected = StorageDirectoryModel::thumbnailUrl(path).path().mid(1);
                if (!cancelled && id == expected) {
                    image = cache->load(path, PreviewCache::HashPolicy::PreviewOnly).image;
                    if (size.isValid() && !image.isNull()) image = image.scaled(size.boundedTo(QSize(512, 512)), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                }
            } catch (...) { image = {}; }
            QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
        });
    }
    void cancel() override { cancelled = true; }
    QQuickTextureFactory *textureFactory() const override { return QQuickTextureFactory::textureFactoryForImage(image); }
private:
    std::atomic_bool cancelled = false;
    QImage image;
};
}
unsigned PreviewCache::workerCount() {
    const auto cores = std::max(1u, std::thread::hardware_concurrency());
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
    return std::min(cores, 2u); // At most two full-resolution decoder working sets on mobile.
#else
    return std::min(cores, 8u);
#endif
}
PreviewCache::PreviewCache(QString directory)
    : m_directory(std::move(directory)) {}
PreviewCache::Result PreviewCache::load(const QString &path) const {
    return load(path, HashPolicy::Required);
}
PreviewCache::Result PreviewCache::load(const QString &path, HashPolicy policy) const {
    Result result;
    const auto url = StorageDirectoryModel::thumbnailUrl(path);
    if (url.isEmpty()) return result;
    const auto drive = FileOperations::containingDrive(path);
    if (!drive) {
        // Native folder pickers may show ordinary external images. Decode them
        // read-only without creating a cache or publishing provider attributes.
        if (policy != HashPolicy::PreviewOnly || !m_directory.isEmpty()
            || !qEnvironmentVariableIsEmpty("IISOCIETY_FILE_CACHE_DIRECTORY")) return result;
        QImageReader reader(path); reader.setAutoTransform(true);
        const auto dimensions = reader.size();
        if (!dimensions.isValid() || qint64(dimensions.width()) * dimensions.height() > 32ll * 1024 * 1024
            || QFileInfo(path).size() > 256ll * 1024 * 1024) return result;
        reader.setScaledSize(dimensions.scaled(QSize(512, 512), Qt::KeepAspectRatio));
        result.image = reader.read();
        if (StorageDirectoryModel::thumbnailUrl(path) != url) return {};
        return result;
    }
    const auto storage = SharedStorage::open(drive->rootPath());
    if (!storage) return result;
    const auto runtime = QDir(drive->rootPath()).filePath(".society-runtime");
    const auto configured = qEnvironmentVariable("IISOCIETY_FILE_CACHE_DIRECTORY");
    const auto cache = configured.isEmpty() ? QDir(runtime).filePath("cache/files") : configured;
    // Validate the configured cache too, even with an explicit preview directory.
    const auto cacheRelative = QDir(runtime).relativeFilePath(cache);
    if (storage->runtimePath(cacheRelative) != cache) return result;
    const auto directory = m_directory.isEmpty() ? QDir(cache).filePath("previews-v1") : m_directory;
    const auto relative = QDir(runtime).relativeFilePath(directory);
    if (storage->runtimePath(relative) != directory) return result;
    const auto key = QCryptographicHash::hash(url.toEncoded(), QCryptographicHash::Sha256).toHex();
    std::lock_guard lock(entryLocks[qHash(key) % entryLocks.size()]);
    const auto prefix = QDir(directory).filePath(QString::fromLatin1(key));
    result.previewPath = prefix + ".png";
    QByteArray previewBytes;
    iiFileProvider::FileDevice metadata(prefix + ".json");
    if (metadata.size() < 4096 && metadata.open(QIODevice::ReadOnly)) {
        const auto record = QJsonDocument::fromJson(metadata.readAll()).object();
        const auto hash = record.value("sha256").toString().toLatin1();
        const auto version = record.value("version").toInt();
        const bool legacy = version == 1 && hash.size() == 64;
        const bool current = version == 2 && (hash.isEmpty() || hash.size() == 64);
        if ((legacy || current) && record.value("source") == url.toString()) {
            iiFileProvider::FileDevice preview(result.previewPath);
            if (preview.size() <= 4 * 1024 * 1024 && preview.open(QIODevice::ReadOnly)) previewBytes = preview.readAll();
            const auto digest = QCryptographicHash::hash(previewBytes, QCryptographicHash::Sha256).toHex();
            const bool intact = legacy || record.value("previewSha256").toString().toLatin1() == digest;
            if (intact) {
                QBuffer buffer(&previewBytes); buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer, "PNG");
                if (reader.size().isValid() && reader.size().width() <= 512 && reader.size().height() <= 512) result.image = reader.read();
            }
            if (!result.image.isNull() && StorageDirectoryModel::thumbnailUrl(path) == url) {
                result.contentHash = hash; result.cacheHit = true;
                if (policy == HashPolicy::PreviewOnly || !hash.isEmpty()) return result;
            }
        }
    }
    metadata.close();
    QImage information;
    if (result.image.isNull()) {
        std::optional<iiFileProvider::native::PresentationRecord> presentation;
        try { presentation = iiFileProvider::native::Presentation::prepare(std::filesystem::path(path.toStdU16String())); } catch (...) {}
        if (presentation) {
            const auto &bytes = presentation->preview.bytes;
            auto image = QImage::fromData(reinterpret_cast<const uchar *>(bytes.data()), qsizetype(bytes.size()));
            if (presentation->preview.kind == "information") information = std::move(image);
            else result.image = std::move(image);
        }
    }
    if (result.image.isNull()) {
        QImageReader reader(path); reader.setAutoTransform(true);
        const auto dimensions = reader.size();
        if (dimensions.isValid() && qint64(dimensions.width()) * dimensions.height() <= 32ll * 1024 * 1024
            && QFileInfo(path).size() <= 256ll * 1024 * 1024) {
            reader.setScaledSize(dimensions.scaled(QSize(512, 512), Qt::KeepAspectRatio));
            result.image = reader.read();
        }
        if (result.image.isNull()) result.image = std::move(information);
    }
    if (result.image.isNull()) return {};
    if (result.image.width() > 512 || result.image.height() > 512) result.image = result.image.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (policy == HashPolicy::Required) {
        iiFileProvider::FileDevice source(path); QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!source.open(QIODevice::ReadOnly) || !hash.addData(&source)) return {};
        result.contentHash = hash.result().toHex();
    }
    if (StorageDirectoryModel::thumbnailUrl(path) != url) return {}; // Concurrent edit: never publish the wrong revision.
    if (storage->ensureRuntimeDirectory(relative) != directory) return result;
    iiFileProvider::FileDevice::setPermissions(directory, iiFileProvider::FileDevice::ReadOwner | iiFileProvider::FileDevice::WriteOwner | iiFileProvider::FileDevice::ExeOwner);
    if (!result.cacheHit) {
        previewBytes.clear(); QBuffer buffer(&previewBytes);
        if (!buffer.open(QIODevice::WriteOnly) || !result.image.save(&buffer, "PNG")) return result;
        iiFileProvider::SaveFile preview(result.previewPath);
        if (!preview.open(QIODevice::WriteOnly) || !preview.setPermissions(iiFileProvider::FileDevice::ReadOwner | iiFileProvider::FileDevice::WriteOwner)
            || preview.write(previewBytes) != previewBytes.size() || !preview.commit()) return result;
    }
    iiFileProvider::SaveFile manifest(prefix + ".json");
    const auto bytes = QJsonDocument(QJsonObject{{"version", 2}, {"source", url.toString()},
        {"sha256", QString::fromLatin1(result.contentHash)},
        {"previewSha256", QString::fromLatin1(QCryptographicHash::hash(previewBytes, QCryptographicHash::Sha256).toHex())}}).toJson(QJsonDocument::Compact);
    if (manifest.open(QIODevice::WriteOnly)) {
        manifest.setPermissions(iiFileProvider::FileDevice::ReadOwner | iiFileProvider::FileDevice::WriteOwner);
        if (manifest.write(bytes) == bytes.size()) manifest.commit();
    }
    static std::atomic_uint writes = 0;
    if (++writes % 32 == 0) prune(directory);
    return result;
}
PreviewProvider::PreviewProvider(QString directory) : m_cache(std::make_shared<PreviewCache>(std::move(directory))) {}
QQuickImageResponse *PreviewProvider::requestImageResponse(const QString &id, const QSize &size) { return new Response(m_cache, id, size); }
}
