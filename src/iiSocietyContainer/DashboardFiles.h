#pragma once

#include <QObject>
#include <QVariantList>
#include <QFileSystemWatcher>
#include <QTimer>
#include "FileActions.h"
#include <atomic>
#include <memory>

// A read-only snapshot of a drive. Scanning never runs on the GUI thread.
namespace iiSocietyContainer {
class IISOCIETY_GUI_EXPORT DashboardFiles : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString containerPath READ containerPath WRITE setContainerPath NOTIFY containerPathChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QVariantList recentFiles READ recentFiles NOTIFY filesChanged)
    Q_PROPERTY(QVariantList generationHistory READ generationHistory NOTIFY filesChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY filesChanged)
public:
    explicit DashboardFiles(QObject *parent = nullptr);
    ~DashboardFiles() override;
    QString containerPath() const { return m_path; }
    void setContainerPath(const QString &path);
    QString query() const { return m_query; }
    void setQuery(const QString &query);
    QVariantList recentFiles() const;
    QVariantList generationHistory() const;
    bool loading() const { return m_loading; }
    QString errorString() const { return m_error; }
    Q_INVOKABLE void refresh();
signals:
    void containerPathChanged();
    void queryChanged();
    void filesChanged();
    void loadingChanged();
private:
    QVariantList filtered(bool history) const;
    QString m_path, m_query, m_error;
    QVariantList m_files;
    bool m_loading = false, m_refreshPending = false;
    quint64 m_revision = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QFileSystemWatcher m_watches;
    QTimer m_debounce;
};

} // namespace iiSocietyContainer
