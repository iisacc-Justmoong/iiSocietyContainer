#include "ModelImportService.h"
#include "ContainerJobStorage.h"

#include <Job.h>
#include <TemporaryLease.h>
#include <QVariantMap>
#include <algorithm>
#include <exception>
#include <utility>

using iiSocietyContainer::ModelImportService;
using iiSocietyContainer::ContainerJobStorage;
using iiSocietyContainer::ModelImportSource;

namespace {
QString text(const std::string &value) { return QString::fromUtf8(value.data(), qsizetype(value.size())); }

QString state(ii::job::JobState value)
{
    using enum ii::job::JobState;
    switch (value) {
    case waiting: return "waiting";
    case waitingNode: return "waitingNode";
    case waitingPermission: return "waitingPermission";
    case running: return "running";
    case partial: return "partial";
    case failed: return "failed";
    case cancelRequested: return "cancelRequested";
    case cancelled: return "cancelled";
    case succeeded: return "succeeded";
    case attention: return "attention";
    }
    return "unknown";
}

QString state(ii::job::ItemState value)
{
    using enum ii::job::ItemState;
    switch (value) {
    case pending: return "pending";
    case running: return "running";
    case succeeded: return "succeeded";
    case failed: return "failed";
    case cancelled: return "cancelled";
    case unknown: return "unknown";
    case skipped: return "skipped";
    }
    return "unknown";
}

QVariantMap describe(const ii::job::Job &job)
{
    return {{"id", text(job.id)}, {"kind", text(job.kind)}, {"resource", text(job.resource)},
        {"state", state(job.state)}, {"stage", text(job.stage)}, {"reason", text(job.reason)},
        {"node", text(job.actualNode)}, {"succeeded", qulonglong(job.succeeded)},
        {"failed", qulonglong(job.failed)}, {"unknown", qulonglong(job.unknown)},
        {"pending", qulonglong(job.pending)}, {"cancelled", qulonglong(job.cancelled)},
        {"total", qulonglong(job.total)}, {"retryCount", qulonglong(job.retryCount)}};
}

QVariantMap describe(const ii::job::Item &item)
{
    return {{"id", text(item.id)}, {"name", text(item.label)}, {"state", state(item.state)},
        {"error", text(item.error)}, {"artifact", text(item.artifact)},
        {"checkpoint", text(item.checkpoint)}, {"attempts", qulonglong(item.attempts)},
        {"retryable", item.retryable}};
}

struct History {
    QVariantList jobs, items;
    QString error, selection, cursor;
    bool more = false, append = false;
};
}

void ModelImportService::refreshJobs() { loadJobHistory(false); }

void ModelImportService::selectJob(const QString &jobId)
{
    m_historySelection = jobId;
    m_historyCursor.clear();
    m_jobItems.clear();
    m_hasMoreJobItems = false;
    emit jobsChanged();
    loadJobHistory(false);
}

void ModelImportService::loadMoreJobItems()
{
    if (!m_hasMoreJobItems || m_jobItems.isEmpty()) return;
    m_historyCursor = m_jobItems.last().toMap().value("id").toString();
    loadJobHistory(true);
}

void ModelImportService::loadJobHistory(bool append)
{
    if (m_historyWorker) { m_historyPending = true; return; }
    const auto result = std::make_shared<History>();
    result->selection = m_historySelection;
    result->cursor = append ? m_historyCursor : QString();
    result->append = append;
    m_historyWorker = QThread::create([result] {
        try {
            ContainerJobStorage storage(ContainerJobStorage::defaultDirectory());
            auto store = storage.open(false);
            if (!store) return;
            auto recover = [&](ii::job::Job job) {
                if (job.state == ii::job::JobState::running || job.state == ii::job::JobState::cancelRequested) {
                    auto lease = storage.tryLease(job.id, false);
                    if (lease && lease->locked()) {
                        store->recoverInterrupted(job.id);
                        job = *store->job(job.id);
                        // All item outcomes may already be durable even if the
                        // old worker stopped just before finishing the job.
                        if (!job.pending && !job.running && !job.unknown) {
                            store->finish(job.id);
                            job = *store->job(job.id);
                        }
                    }
                }
                return job;
            };
            for (const auto &job : store->recent(100))
                if (job.kind == "model-import") result->jobs.append(describe(recover(job)));
            if (result->selection.isEmpty()) return;
            const auto id = result->selection.toUtf8().toStdString();
            if (auto selected = store->job(id)) (void)recover(*selected);
            const auto items = store->items(id, result->cursor.toUtf8().toStdString(), 101);
            result->more = items.size() > 100;
            for (std::size_t index = 0; index < std::min<std::size_t>(items.size(), 100); ++index)
                result->items.append(describe(items[index]));
        } catch (const std::exception &failure) {
            result->error = QString::fromUtf8(failure.what());
        }
    });
    connect(m_historyWorker, &QThread::finished, this, [this, result] {
        m_historyWorker->deleteLater();
        m_historyWorker = nullptr;
        m_jobs = result->jobs;
        m_jobsError = result->error;
        if (m_historySelection == result->selection) {
            if (result->append) m_jobItems.append(result->items);
            else m_jobItems = result->items;
            m_hasMoreJobItems = result->more;
        }
        emit jobsChanged();
        if (std::exchange(m_historyPending, false)) loadJobHistory(false);
    });
    m_historyWorker->start();
}

bool ModelImportService::retryFailed(const QString &jobId)
{
    if (jobId.isEmpty() || busy() || choosingFiles() || m_containerPath.isEmpty()) return false;
    return startImport({}, jobId);
}
