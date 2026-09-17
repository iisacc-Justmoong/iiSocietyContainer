#pragma once

#include <QObject>
#include <QVariantMap>
#include <QFileSystemWatcher>
#include <QTimer>
#include "iiSocietyContainerExport.h"
#include "StorageDirectoryModel.h"
#include <atomic>
#include <memory>

// The four presentation groups share the container's existing model inventory.
namespace iiSocietyContainer {
class IISOCIETYCONTAINER_EXPORT StorageModelCatalog : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString directory READ directory WRITE setDirectory NOTIFY directoryChanged)
    Q_PROPERTY(QVariantMap groups READ groups NOTIFY modelsChanged)
    Q_PROPERTY(int count READ count NOTIFY modelsChanged)
    Q_PROPERTY(int uncategorizedCount READ uncategorizedCount NOTIFY modelsChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY modelsChanged)
    Q_PROPERTY(QString downloadStatus READ downloadStatus NOTIFY downloadStatusChanged)
    Q_PROPERTY(QString requestedPath READ requestedPath NOTIFY downloadStatusChanged)
public:
    explicit StorageModelCatalog(QObject *parent = nullptr);
    ~StorageModelCatalog() override;
    QString directory() const { return m_directory; }
    void setDirectory(const QString &directory);
    QVariantMap groups() const { return m_groups; }
    int count() const { return m_count; }
    int uncategorizedCount() const { return m_uncategorized; }
    bool loading() const { return m_loading; }
    QString errorString() const { return m_error; }
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void activatePath(const QString &path, bool openWhenReady = false);
    QString downloadStatus() const { return m_downloadStatus; }
    QString requestedPath() const { return m_requestedPath; }
signals:
    void directoryChanged();
    void modelsAboutToChange();
    void modelsChanged();
    void loadingChanged();
    void downloadStatusChanged();
    void objectReady(QString path, bool directory);
private:
    QString m_directory, m_error;
    QVariantMap m_groups;
    int m_count = 0, m_uncategorized = 0;
    bool m_loading = false, m_refreshPending = false;
    quint64 m_revision = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QFileSystemWatcher m_files;
    QTimer m_debounce, m_poll;
    StorageDirectoryModel *m_opener = nullptr;
    QString m_downloadStatus, m_requestedPath;
    bool m_openWhenReady = false;
};

}
