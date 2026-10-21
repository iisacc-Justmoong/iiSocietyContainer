#pragma once
#include <FileActions.h>
#include <QImage>
#include <QQuickAsyncImageProvider>
#include <memory>

namespace iiSocietyContainer {
// GUI decoding/cache boundary. Originals and sync/catalog hashes are never changed.
class IISOCIETY_GUI_EXPORT PreviewCache {
public:
    enum class HashPolicy { Required, PreviewOnly };
    struct Result { QImage image; QByteArray contentHash; QString previewPath; bool cacheHit = false; };
    explicit PreviewCache(QString directory = {});
    Result load(const QString &path) const;
    // Visible thumbnails never wait for a full payload hash. An empty
    // contentHash means it has not been requested, not a different hash kind.
    Result load(const QString &path, HashPolicy policy) const;
    static unsigned workerCount();
private:
    QString m_directory;
};
class IISOCIETY_GUI_EXPORT PreviewProvider final : public QQuickAsyncImageProvider {
public:
    explicit PreviewProvider(QString directory = {});
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;
private:
    std::shared_ptr<PreviewCache> m_cache;
};
}
