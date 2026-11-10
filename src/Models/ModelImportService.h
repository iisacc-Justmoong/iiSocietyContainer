#pragma once

#include <iiSocietyContainerExport.h>
#include "ModelImportSource.h"
#include <QObject>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <memory>
#include <stop_token>

namespace iiSocietyContainer {

// Container-owned model operations and durable job history. The host may
// present native pickers separately and supply their granted sources here.
class IISOCIETYCONTAINER_EXPORT ModelImportService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString containerPath READ containerPath WRITE setContainerPath NOTIFY containerPathChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool organizing READ organizing NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY stateChanged)
    Q_PROPERTY(bool choosingFiles READ choosingFiles NOTIFY stateChanged)
    Q_PROPERTY(QString currentJobId READ currentJobId NOTIFY jobsChanged)
    Q_PROPERTY(QString selectedJobId READ selectedJobId NOTIFY jobsChanged)
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
    Q_PROPERTY(QVariantList jobItems READ jobItems NOTIFY jobsChanged)
    Q_PROPERTY(bool hasMoreJobItems READ hasMoreJobItems NOTIFY jobsChanged)
    Q_PROPERTY(QString jobsError READ jobsError NOTIFY jobsChanged)
public:
    explicit ModelImportService(QObject *parent = nullptr);
    ~ModelImportService() override;
    QString containerPath() const;
    void setContainerPath(const QString &path);
    bool busy() const;
    bool organizing() const;
    double progress() const;
    QString status() const;
    QString errorString() const;
    bool choosingFiles() const { return m_choosingFiles; }
    void setChoosingFiles(bool choosing);
    bool importSources(const QList<ModelImportSource> &sources);
    QString currentJobId() const { return m_currentJobId; }
    QString selectedJobId() const { return m_historySelection; }
    QVariantList jobs() const { return m_jobs; }
    QVariantList jobItems() const { return m_jobItems; }
    bool hasMoreJobItems() const { return m_hasMoreJobItems; }
    QString jobsError() const { return m_jobsError; }

    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool organizeModels();
    Q_INVOKABLE bool recategorize(const QString &path, const QString &category);
    Q_INVOKABLE void refreshJobs();
    Q_INVOKABLE void selectJob(const QString &jobId);
    Q_INVOKABLE void loadMoreJobItems();
    Q_INVOKABLE bool retryFailed(const QString &jobId);

signals:
    void containerPathChanged();
    void stateChanged();
    void jobsChanged();
    void finished(const QString &containerPath, const QStringList &paths);
    void organized(const QString &containerPath, const QStringList &paths);
    void recategorized(const QString &containerPath, const QString &previousPath, const QString &path);

protected:
    void reportInputError(const QString &error);
private:
    QString m_containerPath;
    QString m_status;
    QString m_error;
    QString m_organizationStatus, m_organizationError;
    double m_progress = 0;
    bool m_choosingFiles = false;
    bool m_needsOrganization = false;
    QThread *m_worker = nullptr;
    QThread *m_organizationWorker = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    std::shared_ptr<std::atomic_bool> m_organizationCancelled;
    std::stop_source m_recordingCancellation;
    std::stop_source m_organizationCancellation;
    QThread *m_historyWorker = nullptr;
    QString m_currentJobId, m_historySelection, m_historyCursor, m_jobsError;
    QVariantList m_jobs, m_jobItems;
    bool m_hasMoreJobItems = false, m_historyPending = false;
    bool startImport(QList<ModelImportSource> sources, const QString &existingJob);
    void loadJobHistory(bool append);
    void yieldOrganization();
};
}
