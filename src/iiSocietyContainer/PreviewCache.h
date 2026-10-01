#pragma once
#include <FileActions.h>
#include <QImage>
#include <QQuickAsyncImageProvider>
#include <memory>

namespace iiSocietyContainer {
// GUI decoding/cache boundary. Originals and sync/catalog hashes are never changed.
class IISOCIETY_GUI_EXPORT PreviewCache {
public:
    struct Result { QImage image; QByteArray contentHash; QString previewPath; bool cacheHit = false; };
    explicit PreviewCache(QString directory = {});
    Result load(const QString &path) const;
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
