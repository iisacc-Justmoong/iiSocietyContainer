#include "PreviewCache.h"
#include <StorageDirectoryModel.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
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
        if (total > cacheBudget) QFile::remove(file.absoluteFilePath());
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
    ~Workers() { { std::lock_guard lock(mutex); stopping = true; } ready.notify_all(); }
    void submit(std::function<void()> work) {
        { std::lock_guard lock(mutex); queue.push_back(std::move(work)); } ready.notify_one();
    }
private:
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> queue;
    bool stopping = false;
    // Destroy/join before the queue and its synchronization primitives.
    std::vector<std::jthread> threads;
};
Workers &workers() { static Workers pool; return pool; }
class Response final : public QQuickImageResponse {
public:
    Response(std::shared_ptr<PreviewCache> cache, QString id, QSize size) {
        workers().submit([this, cache = std::move(cache), id = std::move(id), size] {
            try {
                const auto path = QString::fromUtf8(QByteArray::fromBase64(id.section('/', 0, 0).toLatin1(), QByteArray::Base64UrlEncoding));
                const auto expected = StorageDirectoryModel::thumbnailUrl(path).path().mid(1);
                if (!cancelled && id == expected) {
                    image = cache->load(path).image;
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
    : m_directory(directory.isEmpty() ? QDir(StorageDirectoryModel::cacheDirectory()).filePath("previews-v1") : std::move(directory)) {}
PreviewCache::Result PreviewCache::load(const QString &path) const {
    Result result;
    const auto url = StorageDirectoryModel::thumbnailUrl(path);
    if (url.isEmpty()) return result;
    const auto key = QCryptographicHash::hash(url.toEncoded(), QCryptographicHash::Sha256).toHex();
    std::lock_guard lock(entryLocks[qHash(key) % entryLocks.size()]);
    const auto prefix = QDir(m_directory).filePath(QString::fromLatin1(key));
    result.previewPath = prefix + ".png";
    QFile metadata(prefix + ".json");
    if (metadata.size() < 4096 && metadata.open(QIODevice::ReadOnly)) {
        const auto record = QJsonDocument::fromJson(metadata.readAll()).object();
        const auto hash = record.value("sha256").toString().toLatin1();
        if (record.value("version") == 1 && hash.size() == 64 && record.value("source") == url.toString()) {
            QImageReader reader(result.previewPath);
            if (reader.size().isValid() && reader.size().width() <= 512 && reader.size().height() <= 512) result.image = reader.read();
            if (!result.image.isNull() && StorageDirectoryModel::thumbnailUrl(path) == url) {
                result.contentHash = hash; result.cacheHit = true; return result;
            }
        }
    }
    metadata.close();
    QImageReader reader(path); reader.setAutoTransform(true);
    const auto dimensions = reader.size();
    if (!dimensions.isValid() || qint64(dimensions.width()) * dimensions.height() > 32ll * 1024 * 1024
        || QFileInfo(path).size() > 256ll * 1024 * 1024) return {};
    reader.setScaledSize(dimensions.scaled(QSize(512, 512), Qt::KeepAspectRatio));
    result.image = reader.read();
    if (result.image.isNull()) return {};
    if (result.image.width() > 512 || result.image.height() > 512) result.image = result.image.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QFile source(path); QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!source.open(QIODevice::ReadOnly) || !hash.addData(&source)) return {};
    result.contentHash = hash.result().toHex();
    if (StorageDirectoryModel::thumbnailUrl(path) != url) return {}; // Concurrent edit: never publish the wrong revision.
    if (!QDir().mkpath(m_directory) || QFileInfo(m_directory).isSymLink()) return result;
    QFile::setPermissions(m_directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QSaveFile preview(result.previewPath);
    if (!preview.open(QIODevice::WriteOnly) || !preview.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        || !result.image.save(&preview, "PNG") || !preview.commit()) return result;
    QSaveFile manifest(prefix + ".json");
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"source", url.toString()},
        {"sha256", QString::fromLatin1(result.contentHash)}}).toJson(QJsonDocument::Compact);
    if (manifest.open(QIODevice::WriteOnly)) {
        manifest.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        if (manifest.write(bytes) == bytes.size()) manifest.commit();
    }
    static std::atomic_uint writes = 0;
    if (++writes % 32 == 1) prune(m_directory);
    return result;
}
PreviewProvider::PreviewProvider(QString directory) : m_cache(std::make_shared<PreviewCache>(std::move(directory))) {}
QQuickImageResponse *PreviewProvider::requestImageResponse(const QString &id, const QSize &size) { return new Response(m_cache, id, size); }
}
