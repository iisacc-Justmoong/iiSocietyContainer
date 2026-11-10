#include <iiFileProviderDevice.h>
#include "ModelImportService.h"
#include "ContainerJobStorage.h"
#include "../AssetCompanions.h"

#include <Job.h>
#include <SocietyDrive.h>
#include <SharedStorage.h>
#include <ModelStore.h>
#include <FileRepository.h>
#include <FileCatalog.h>
#include <StorageMap.h>
#include <TemporaryLease.h>
#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <utility>
#ifdef Q_OS_ANDROID
#include <AndroidStorage.h>
#endif

namespace {
using iiSocietyContainer::SocietyDrive;
using iiSocietyContainer::StoreSection;
using iiSocietyContainer::ModelImportSource;
using iiSocietyContainer::ModelImportService;
using iiSocietyContainer::ContainerJobStorage;

bool isModelUrl(const QUrl &url)
{
#ifdef Q_OS_ANDROID
    if (url.scheme() == "content") {
        const auto suffix = QFileInfo(iiSocietyContainer::androidDocumentName(url.toString())).suffix();
        return suffix.compare("safetensor", Qt::CaseInsensitive) == 0 || suffix.compare("safetensors", Qt::CaseInsensitive) == 0;
    }
#endif
    const auto path = url.toLocalFile();
    const auto suffix = QFileInfo(path).suffix();
    return url.isLocalFile() && url.host().isEmpty() && !url.hasQuery() && !url.hasFragment()
        && QDir::isAbsolutePath(path)
        && (suffix.compare("safetensor", Qt::CaseInsensitive) == 0
            || suffix.compare("safetensors", Qt::CaseInsensitive) == 0);
}

struct ImportResult {
    QStringList paths;
    QStringList errors;
    QString lastCopiedPath;
    int copied = 0;
    int existing = 0;
};

std::string bytes(const QString &value) { return value.toUtf8().toStdString(); }
QString display(const std::string &value) { return QString::fromUtf8(value.data(), qsizetype(value.size())); }

QString itemId(qsizetype index) { return QStringLiteral("item-%1").arg(index, 12, 10, QChar('0')); }

QByteArray hashFile(const QString &path, const std::shared_ptr<std::atomic_bool> &cancelled, QString *error)
{
    iiFileProvider::FileDevice file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = file.errorString(); return {}; }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (!cancelled->load()) {
        const auto count = file.read(buffer.data(), buffer.size());
        if (count < 0) { *error = file.errorString(); return {}; }
        if (!count) return digest.result().toHex();
        digest.addData(QByteArrayView(buffer.constData(), count));
    }
    *error = QObject::tr("Cancelled while verifying the saved model.");
    return {};
}

std::string publishedCheckpoint(const QString &path, const QByteArray &hash)
{
    return QJsonDocument(QJsonObject{{"phase", "published"}, {"path", path},
        {"sha256", QString::fromLatin1(hash)}}).toJson(QJsonDocument::Compact).toStdString();
}

QString verifiedPublishedPath(const std::string &checkpoint, const QString &models,
                              const std::shared_ptr<std::atomic_bool> &cancelled, QString *error)
{
    const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(checkpoint)).object();
    if (object.value("phase").toString() != "published") return {};
    const auto path = object.value("path").toString();
    const auto expected = object.value("sha256").toString().toLatin1();
    const QFileInfo file(path), root(models);
    const auto canonical = file.canonicalFilePath();
    if (file.isSymLink() || !file.isFile() || canonical.isEmpty() || root.canonicalFilePath().isEmpty()
        || !canonical.startsWith(root.canonicalFilePath() + '/') || expected.size() != 64) {
        *error = QObject::tr("The saved model is unavailable or no longer belongs to this drive.");
        return {};
    }
    const auto actual = hashFile(canonical, cancelled, error);
    if (!error->isEmpty()) return {};
    if (actual != expected) { *error = QObject::tr("The saved model changed since this job recorded it."); return {}; }
    return canonical;
}

bool repositoryContended(const QString &error)
{
    for (const auto &reason : {QStringLiteral("another repository operation is still active"),
                               QStringLiteral("database is locked")})
        if (error == reason || error.endsWith(QStringLiteral(": ") + reason)) return true;
    return false;
}
}

ModelImportService::ModelImportService(QObject *parent) : QObject(parent) {}
ModelImportService::~ModelImportService()
{
    cancel();
    if (m_worker) {
        m_worker->wait();
        delete m_worker;
    }
    if (m_organizationWorker) {
        m_organizationWorker->wait();
        delete m_organizationWorker;
    }
    if (m_historyWorker) {
        m_historyWorker->wait();
        delete m_historyWorker;
    }
}
QString ModelImportService::containerPath() const { return m_containerPath; }
void ModelImportService::setContainerPath(const QString &path)
{
    if (m_containerPath == path)
        return;
    cancel();
    m_containerPath = path;
    m_needsOrganization = !path.isEmpty();
    m_organizationStatus.clear();
    m_organizationError.clear();
    if (!busy()) {
        m_status.clear();
        m_error.clear();
        emit stateChanged();
    }
    emit containerPathChanged();
    refreshJobs();
    QTimer::singleShot(0, this, [this] { if (m_needsOrganization && !busy() && !organizing()) organizeModels(); });
}

bool ModelImportService::organizeModels()
{
    if (busy() || organizing() || choosingFiles() || m_containerPath.isEmpty()) return false;
    const auto root = m_containerPath;
    m_needsOrganization = false;
    m_organizationCancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_organizationCancelled;
    m_organizationCancellation = std::stop_source{};
    const auto stop = m_organizationCancellation.get_token();
    const auto report = std::make_shared<iiSocietyContainer::ModelOrganization>();
    m_organizationError.clear();
    m_organizationStatus = tr("Organizing models by type…");
    m_organizationWorker = QThread::create([root, report, cancelled, stop] {
        QString error;
        const auto store = iiSocietyContainer::ModelStore::open(root, &error);
        if (store) *report = store->organize(cancelled.get(), stop);
        else report->errors.append(error);
    });
    connect(m_organizationWorker, &QThread::finished, this, [this, root, report] {
        m_organizationWorker->deleteLater(); m_organizationWorker = nullptr; m_organizationCancelled.reset();
        if (m_containerPath == root) {
            m_organizationError = report->cancelled ? QString() : report->errors.join('\n');
            m_organizationStatus = report->cancelled ? tr("Model organization cancelled.") : tr("Organized %1 model(s) by type.").arg(report->moved.size());
        }
        QStringList paths;
        for (const auto &move : report->moved) paths.append(move.path);
        emit stateChanged();
        emit organized(root, paths);
        if (m_needsOrganization && !busy()) QTimer::singleShot(0, this, [this] { organizeModels(); });
    });
    m_organizationWorker->start(QThread::LowPriority); emit stateChanged(); return true;
}

bool ModelImportService::recategorize(const QString &path, const QString &category)
{
    const auto type = iiSocietyContainer::modelTypeFromName(category);
    if (busy()) { reportInputError(tr("Another model operation is running. Try again when it finishes.")); return false; }
    if (choosingFiles() || m_containerPath.isEmpty() || !type || path.isEmpty()) return false;
    yieldOrganization();
    const auto root = m_containerPath;
    const auto result = std::make_shared<QPair<QString, QString>>();
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_cancelled;
    m_recordingCancellation = std::stop_source{};
    const auto recordingStop = m_recordingCancellation.get_token();
    m_error.clear();
    m_progress = 0;
    m_status = tr("Moving model to %1…").arg(iiSocietyContainer::modelTypeName(*type));
    m_worker = QThread::create([this, root, path, type, result, cancelled, recordingStop] {
        QString error;
        const auto drive = SocietyDrive::open(root, &error);
        if (!drive) { result->second = error; return; }
        if (!iiSocietyContainer::StorageMap(*drive).isLocalAuthority()) {
            result->second = tr("Change this model's category on its Society host."); return;
        }
        const auto store = iiSocietyContainer::ModelStore::open(root, &error);
        const auto storage = iiSocietyContainer::SharedStorage::open(root, &error);
        if (!store || !storage) { result->second = error; return; }
        if (cancelled->load()) return;
        const auto models = drive->sectionPath(StoreSection::Models);
        const auto oldRelative = QDir(models).relativeFilePath(path);
        std::string oldKey;
        try { oldKey = storage->managedFilePath(StoreSection::Models, oldRelative); }
        catch (const std::exception &e) { result->second = QString::fromUtf8(e.what()); return; }
        const auto report = [this, root](const QString &status) {
            QMetaObject::invokeMethod(this, [this, root, status] {
                if (m_containerPath != root) return;
                m_status = status;
                emit stateChanged();
            }, Qt::QueuedConnection);
        };
        report(tr("Waiting for the managed library before moving %1…").arg(QFileInfo(path).fileName()));
        while (!cancelled->load() && !recordingStop.stop_requested()) {
            bool mutationCompleted = false;
            try {
                auto managed = storage->fileRepository({}, iiSocietyContainer::FileRepository::Access::ReadWrite,
                                                       iiSocietyContainer::FileRepository::Intent::UserJob);
                const bool completed = managed->tryUserJobOperation([&](std::stop_token operationStop) {
                    if (cancelled->load() || operationStop.stop_requested()) return;
                    report(tr("Checking %1 before moving…").arg(QFileInfo(path).fileName()));
                    const auto companions = iiSocietyContainer::ModelClassifier::companionFiles(path);
                    QList<std::string> companionKeys;
                    for (const auto &companion : companions)
                        companionKeys.append(storage->managedFilePath(StoreSection::Models, QDir(models).relativeFilePath(companion)));
                    // Capture changed content before a rename; unchanged tracked
                    // models only require metadata checks, not another full read.
                    const auto prepare = [&](const std::string &key) {
                        if (managed->lookup(key)) managed->observe(key, {}, operationStop);
                    };
                    if (QFileInfo(path).isDir()) {
                        std::int64_t cursor = 0;
                        for (;;) {
                            if (cancelled->load() || operationStop.stop_requested()) return;
                            const auto page = managed->objects().index(cursor, 1024);
                            if (page.empty()) break;
                            for (const auto &file : page) {
                                if (cancelled->load() || operationStop.stop_requested()) return;
                                cursor = file.indexKey;
                                if (file.path.starts_with(oldKey + '/')) prepare(file.path);
                            }
                        }
                    } else prepare(oldKey);
                    for (const auto &key : companionKeys) prepare(key);
                    if (cancelled->load() || operationStop.stop_requested()) return;
                    // Once a native move begins, complete its index update even
                    // if a foreground edit arrives. No retry may repeat it.
                    report(tr("Moving model to %1…").arg(iiSocietyContainer::modelTypeName(*type)));
                    result->first = store->place(path, *type, &error);
                    if (result->first.isEmpty()) { result->second = error; return; }
                    if (result->first == path) { mutationCompleted = true; return; }
                    report(tr("Updating the managed library for %1…").arg(QFileInfo(result->first).fileName()));
                    try {
                        const auto newKey = storage->managedFilePath(StoreSection::Models, QDir(models).relativeFilePath(result->first));
                        managed->relocated(oldKey, newKey, true);
                        if (QFileInfo(result->first).isFile() && store->indexAsset(result->first, &error).isEmpty())
                            throw std::runtime_error(error.toStdString());
                        for (qsizetype i = 0; i < companions.size(); ++i) {
                            const auto fullPrefix = companions[i].startsWith(path + '.');
                            const auto oldPrefix = fullPrefix ? path : QDir(QFileInfo(path).absolutePath()).filePath(QFileInfo(path).completeBaseName());
                            const auto newPrefix = fullPrefix ? result->first : QDir(QFileInfo(result->first).absolutePath()).filePath(QFileInfo(result->first).completeBaseName());
                            const auto destination = newPrefix + companions[i].mid(oldPrefix.size());
                            managed->relocated(companionKeys[i], storage->managedFilePath(StoreSection::Models, QDir(models).relativeFilePath(destination)), true);
                            if (QFileInfo(destination).isFile() && store->indexAsset(destination, &error).isEmpty())
                                throw std::runtime_error(error.toStdString());
                        }
                        mutationCompleted = true;
                    } catch (const std::exception &failure) {
                        result->second = tr("The model moved, but its managed index needs synchronization: %1")
                            .arg(QString::fromUtf8(failure.what()));
                    }
                }, recordingStop);
                if (!completed && result->first.isEmpty() && result->second.isEmpty()
                    && !cancelled->load() && !recordingStop.stop_requested()) {
                    report(tr("Waiting for a file edit before moving %1…").arg(QFileInfo(path).fileName()));
                    continue;
                }
                return;
            } catch (const std::exception &e) {
                const auto message = QString::fromUtf8(e.what());
                if (mutationCompleted && !result->first.isEmpty() && result->second.isEmpty()) return;
                if (!result->first.isEmpty()) {
                    result->second = tr("The model moved, but its managed index needs synchronization: %1").arg(message);
                    return;
                }
                if (cancelled->load() || recordingStop.stop_requested()) return;
                if (!repositoryContended(message)) { result->second = message; return; }
                report(tr("The managed library is busy. Waiting to retry %1…").arg(QFileInfo(path).fileName()));
                QThread::msleep(100);
            }
        }
    });
    connect(m_worker, &QThread::finished, this, [this, root, path, result, cancelled] {
        m_worker->deleteLater(); m_worker = nullptr; m_cancelled.reset();
        if (m_containerPath == root) {
            m_error = result->second;
            m_progress = result->first.isEmpty() ? 0 : result->second.isEmpty() ? 1 : 0.9;
            m_status = cancelled->load() && result->first.isEmpty() ? tr("Category change cancelled.")
                : result->first.isEmpty() ? tr("Could not change the model category.")
                : !result->second.isEmpty() ? tr("Model moved; library synchronization needs attention.")
                : tr("Model category updated.");
            emit stateChanged();
            if (!result->first.isEmpty()) emit recategorized(root, path, result->first);
        }
        // Leave pending organization for the next explicit operation after a
        // cancellation. A newly selected container still needs its own scan.
        if (m_needsOrganization && (!cancelled->load() || m_containerPath != root))
            QTimer::singleShot(0, this, [this] { organizeModels(); });
    });
    m_worker->start(); emit stateChanged(); return true;
}
bool ModelImportService::busy() const { return m_worker != nullptr; }
bool ModelImportService::organizing() const { return m_organizationWorker != nullptr; }
double ModelImportService::progress() const { return m_progress; }
QString ModelImportService::status() const {
    // Keep the latest user-requested result visible if automatic organization
    // resumes after that operation has finished (including cancellation).
    return !busy() && organizing() && m_status.isEmpty() ? m_organizationStatus : m_status;
}
QString ModelImportService::errorString() const { return m_error.isEmpty() ? m_organizationError : m_error; }
void ModelImportService::reportInputError(const QString &error)
{
    m_error = error;
    m_status = busy() ? error : QString();
    emit stateChanged();
}
void ModelImportService::setChoosingFiles(bool choosing)
{
    if (m_choosingFiles == choosing) return;
    m_choosingFiles = choosing;
    emit stateChanged();
    if (!choosing && m_needsOrganization && !busy() && !organizing())
        QTimer::singleShot(0, this, [this] { if (m_needsOrganization) organizeModels(); });
}
bool ModelImportService::importSources(const QList<ModelImportSource> &sources)
{
    if (busy()) { reportInputError(tr("Another model operation is running. Try again when it finishes.")); return false; }
    if (choosingFiles() || m_containerPath.isEmpty() || sources.isEmpty())
        return false;
    for (const auto &source : sources) {
        if (!source.read || QFileInfo(source.name).fileName() != source.name
            || !isModelUrl(QUrl::fromLocalFile(QDir(m_containerPath).filePath(source.name)))) {
            m_error = tr("The file provider must supply a .safetensor or .safetensors filename.");
            emit stateChanged();
            return false;
        }
    }
    return startImport(sources, {});
}

bool ModelImportService::startImport(QList<ModelImportSource> sources, const QString &existingJob)
{
    if (busy() || choosingFiles() || m_containerPath.isEmpty()) return false;
    yieldOrganization();
    const auto root = m_containerPath;
    const auto jobId = existingJob.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : existingJob;
    m_currentJobId = jobId;
    m_historySelection = jobId;
    m_historyCursor.clear();
    m_jobItems.clear();
    emit jobsChanged();
    const auto result = std::make_shared<ImportResult>();
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_cancelled = cancelled;
    m_recordingCancellation = std::stop_source{};
    m_progress = 0;
    m_error.clear();
    m_status = tr("Preparing models…");
    m_worker = QThread::create([this, root, sources, result, cancelled, jobId, existingJob]() mutable {
      try {
        ContainerJobStorage jobStorage(ContainerJobStorage::defaultDirectory());
        auto jobs = jobStorage.open(true);
        auto lease = jobStorage.tryLease(bytes(jobId), true);
        if (!lease || !lease->locked()) {
            result->errors.append(tr("This job is already running in another Society process."));
            return;
        }
        QString driveError;
        const auto drive = SocietyDrive::open(root, &driveError);
        if (!drive) {
            result->errors.append(driveError);
            return;
        }
        const auto driveId = drive->storageKind() == SocietyDrive::StorageKind::LogicalDisk
            ? drive->storageIdentifier() : drive->identifier();
        if (driveId.isEmpty()) { result->errors.append(tr("The Society drive has no stable identity.")); return; }
        const auto node = bytes(QStringLiteral("local:") + QSysInfo::machineHostName());
        std::vector<std::string> itemIds;
        if (existingJob.isEmpty()) {
            ii::job::JobRequest request{bytes(jobId), "model-import", "local-user:unverified", bytes(driveId), "", node, true};
            std::vector<ii::job::ItemRequest> items;
            items.reserve(std::size_t(sources.size()));
            for (qsizetype index = 0; index < sources.size(); ++index) {
                const auto id = bytes(itemId(index));
                itemIds.push_back(id);
                items.push_back({id, bytes(sources[index].resumablePath), "", "", bytes(sources[index].name)});
            }
            jobs->create(request, items);
        } else {
            auto job = jobs->job(bytes(jobId));
            if (!job || job->kind != "model-import" || job->resource != bytes(driveId)) {
                result->errors.append(tr("This job belongs to a different Society drive."));
                return;
            }
            if (job->state == ii::job::JobState::running || job->state == ii::job::JobState::cancelRequested) {
                jobs->recoverInterrupted(bytes(jobId));
                job = jobs->job(bytes(jobId));
            }
            std::vector<ii::job::Item> selected;
            std::string cursor;
            for (;;) {
                const auto page = jobs->items(bytes(jobId), cursor, 1024);
                if (page.empty()) break;
                for (const auto &item : page) {
                    cursor = item.id;
                    if (item.state == ii::job::ItemState::pending ||
                        (item.state == ii::job::ItemState::failed && item.retryable)) selected.push_back(item);
                }
            }
            if (job->state == ii::job::JobState::partial || job->state == ii::job::JobState::failed ||
                job->state == ii::job::JobState::attention || job->state == ii::job::JobState::cancelled) {
                const auto retried = jobs->retryFailed(bytes(jobId));
                if (!retried && job->state == ii::job::JobState::attention && job->pending)
                    jobs->resumePending(bytes(jobId));
            }
            sources.clear();
            for (const auto &item : selected) {
                const auto path = display(item.source);
                itemIds.push_back(item.id);
                sources.append({display(item.label), [path](const auto &consume, const auto &) {
                    if (path.isEmpty()) return QObject::tr("Select this item again; its provider access was temporary.");
                    consume(path);
                    return QString();
                }, path});
            }
            if (sources.isEmpty()) {
                result->errors.append(job->unknown
                    ? tr("Some results need verification before they can be retried.")
                    : tr("There are no retryable failed items in this job."));
                return;
            }
        }
        jobs->begin(bytes(jobId), node, "preparing models");
        const auto models = drive->sectionPath(StoreSection::Models);
        const auto store = iiSocietyContainer::ModelStore::open(root, &driveError);
        if (!store || !store->ensureLayout(&driveError)) {
            if (driveError.isEmpty()) driveError = tr("The model storage is unavailable.");
            result->errors.append(driveError);
            jobs->setWaiting(bytes(jobId), ii::job::JobState::waiting, bytes(driveError));
            return;
        }
        std::unique_ptr<iiFileProvider::FileCatalog> existingCatalog;
        const auto catalogReserved = [&](const QString &path) {
            const auto rootPath = std::filesystem::path(models.toStdU16String());
            if (!std::filesystem::exists(rootPath / ".iisacc-file-provider/catalog.sqlite3")) return false;
            if (!existingCatalog)
                existingCatalog = std::make_unique<iiFileProvider::FileCatalog>(rootPath, iiFileProvider::FileCatalog::Access::ReadOnly);
            const auto relative = QDir(models).relativeFilePath(path);
            if (existingCatalog->lookupPath(relative.toUtf8().toStdString())) return true;
#ifdef Q_OS_MACOS
            const auto decomposed = relative.normalized(QString::NormalizationForm_D);
            if (decomposed != relative && existingCatalog->lookupPath(decomposed.toUtf8().toStdString())) return true;
#endif
            return false;
        };
        QSet<QString> seen;
        qsizetype completed = 0;
        bool publicationBlocked = false;
        QElapsedTimer throttle;
        throttle.start();
        const auto reportProgress = [&](const QString &name, double fileProgress, bool force = false) {
            if (!force && throttle.elapsed() < 80)
                return;
            throttle.restart();
            // Publication and the small metadata registration follow the copy.
            const double fraction = std::clamp((completed + 0.98 * fileProgress) / sources.size(), 0.0, 1.0);
            const auto itemNumber = completed + 1;
            const auto itemCount = sources.size();
            const int itemPercent = qRound(std::clamp(fileProgress, 0.0, 1.0) * 100);
            QMetaObject::invokeMethod(this, [this, name, fraction, itemNumber, itemCount, itemPercent] {
                m_progress = fraction;
                m_status = tr("Copying %1 (%2 of %3)… %4% (overall %5%)")
                    .arg(name).arg(itemNumber).arg(itemCount).arg(itemPercent).arg(qRound(fraction * 100));
                emit stateChanged();
            }, Qt::QueuedConnection);
        };
        const auto recordModel = [&](const QString &path, const QString &name, QString *error) {
            const double fraction = std::clamp((completed + 0.98) / sources.size(), 0.0, 1.0);
            QMetaObject::invokeMethod(this, [this, name, fraction] {
                m_progress = fraction;
                m_status = tr("Indexing %1 in the model library…").arg(name);
                emit stateChanged();
            }, Qt::QueuedConnection);
            if (cancelled->load()) return false;
            return !store->indexAsset(path, error).isEmpty();
        };
        const auto recordObject = [&](const QString &path, const QString &name, QString *error) {
            if (!recordModel(path, name, error)) return false;
            for (const auto &companion : iiSocietyContainer::ModelClassifier::companionFiles(path))
                if (!recordModel(companion, QFileInfo(companion).fileName(), error)) return false;
            return true;
        };
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        for (qsizetype sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
            const auto &entry = sources[sourceIndex];
            if (cancelled->load())
                break;
            const auto &item = itemIds[std::size_t(sourceIndex)];
            const auto claim = jobs->claim(bytes(jobId), item);
            if (publicationBlocked) {
                jobs->fail(bytes(jobId), item, claim.token,
                           "Not started because the container publication lock is unavailable.",
                           !entry.resumablePath.isEmpty());
                ++completed;
                jobs->progress(bytes(jobId), "importing models", std::uint64_t(completed),
                               std::uint64_t(sources.size()), "items");
                continue;
            }
            const auto previous = QJsonDocument::fromJson(QByteArray::fromStdString(claim.previousCheckpoint)).object();
            bool published = previous.value("phase").toString() == "published";
            bool canRetry = !entry.resumablePath.isEmpty() || published;
            bool consumed = false;
            bool duplicate = false;
            const auto errorsBefore = result->errors.size();
            const auto pathsBefore = result->paths.size();
            // Native providers may need to download or materialize a file first.
            QMetaObject::invokeMethod(this, [this, name = entry.name] {
                m_status = tr("Preparing %1…").arg(name);
                emit stateChanged();
            }, Qt::QueuedConnection);
            QString readError;
            std::exception_ptr callbackFailure;
            std::mutex callbackMutex;
            if (!claim.previousCheckpoint.empty())
                jobs->checkpoint(bytes(jobId), item, claim.token, claim.previousCheckpoint);
            if (published) {
                QString error;
                const auto path = verifiedPublishedPath(claim.previousCheckpoint, models, cancelled, &error);
                if (path.isEmpty()) {
                    canRetry = false;
                    result->errors.append(tr("%1: %2").arg(entry.name, error));
                } else if (!recordObject(path, entry.name, &error)) {
                    if (!cancelled->load()) result->errors.append(tr("%1: %2").arg(entry.name, error));
                } else {
                    result->paths.append(path);
                    ++result->existing;
                }
            } else readError = entry.read([&](const QString &path) {
              std::lock_guard guard(callbackMutex);
              try {
                if (std::exchange(consumed, true)) {
                    result->errors.append(tr("%1: the provider returned more than one file for one item.").arg(entry.name));
                    return;
                }
                QString actualPath = path;
                if (actualPath.startsWith(models + '/') && !QFileInfo::exists(actualPath)) {
                    const auto resolved = store->resolve(QDir(models).relativeFilePath(actualPath));
                    if (!resolved.isEmpty()) actualPath = resolved;
                }
                const QFileInfo input(actualPath);
                const QString name = entry.name;
                bool content = false;
#ifdef Q_OS_ANDROID
                content = QUrl(path).scheme() == "content";
#endif
                if (!isModelUrl(QUrl::fromLocalFile(QDir(models).filePath(name)))
                    || QFileInfo(name).fileName() != name || (!content && (!input.isFile()
                    || input.isSymLink() || !input.isReadable()))) {
                    result->errors.append(tr("%1: the source must be a readable .safetensor or .safetensors file.").arg(name));
                    return;
                }
                if (cancelled->load())
                    return;
                if (!content && previous.value("phase").toString() == "copying" &&
                    (previous.value("sourcePath").toString() != input.canonicalFilePath() ||
                     previous.value("sourceSize").toString() != QString::number(input.size()) ||
                     previous.value("sourceModifiedMs").toString() != QString::number(input.lastModified().toMSecsSinceEpoch()))) {
                    canRetry = false;
                    result->errors.append(tr("%1: the source changed since this job began copying it. Start a new import.").arg(name));
                    return;
                }
                if (!content) {
                    const auto validation = iiSocietyContainer::ModelClassifier::validateSafetensors(actualPath);
                    if (!validation.isEmpty()) {
                        result->errors.append(tr("%1: incomplete or invalid model (%2). Download the complete file and try again.").arg(name, validation));
                        return;
                    }
                }
                const auto canonical = content ? path : input.canonicalFilePath();
                if (seen.contains(canonical)) {
                    duplicate = true;
                    return;
                }
                seen.insert(canonical);
                // Revalidate the original drive identity and section paths before each write.
                if (!drive->isValid()) {
                    result->errors.append(tr("The Society drive changed or Models is unavailable."));
                    return;
                }
                if (canonical.startsWith(models + '/')) {
                    QString error;
                    const auto hash = hashFile(canonical, cancelled, &error);
                    if (!error.isEmpty()) { if (!cancelled->load()) result->errors.append(error); return; }
                    const auto placed = store->place(canonical, {}, &error);
                    if (placed.isEmpty()) result->errors.append(error);
                    else {
                        jobs->checkpoint(bytes(jobId), item, claim.token, publishedCheckpoint(placed, hash));
                        published = true;
                        canRetry = true;
                        if (!recordObject(placed, name, &error)) { if (!cancelled->load()) result->errors.append(error); }
                        else { result->paths.append(placed); ++result->existing; }
                    }
                    return;
                }
                iiFileProvider::FileDevice source(content ? path : input.absoluteFilePath());
                std::optional<iiFileProvider::TemporaryLease> activity;
                try {
                    activity.emplace(std::filesystem::path(models.toStdU16String()),
                        iiFileProvider::TemporaryLease::Mode::Shared);
                } catch (const std::exception &failure) {
                    result->errors.append(tr("%1: %2").arg(name, QString::fromUtf8(failure.what())));
                    return;
                }
                iiFileProvider::TemporaryFile copy(QDir(models).filePath(".society-import-XXXXXX"));
                if (!source.open(QIODevice::ReadOnly) || !copy.open()) {
                    result->errors.append(tr("%1: %2").arg(name, source.isOpen() ? copy.errorString() : source.errorString()));
                    return;
                }
                QString error;
                qint64 copied = 0;
                qint64 checkpointAt = 64 * 1024 * 1024;
                QCryptographicHash digest(QCryptographicHash::Sha256);
                const auto copyCheckpoint = [&](qint64 count) {
                    QJsonObject value{{"phase", "copying"}, {"bytes", QString::number(count)}};
                    if (!content) {
                        value.insert("sourcePath", canonical);
                        value.insert("sourceSize", QString::number(input.size()));
                        value.insert("sourceModifiedMs", QString::number(input.lastModified().toMSecsSinceEpoch()));
                    }
                    return QJsonDocument(value).toJson(QJsonDocument::Compact).toStdString();
                };
                jobs->checkpoint(bytes(jobId), item, claim.token, copyCheckpoint(0));
                reportProgress(name, 0, true);
                while (!cancelled->load()) {
                    const auto count = source.read(buffer.data(), buffer.size());
                    if (count < 0) {
                        error = source.errorString();
                        break;
                    }
                    if (count == 0)
                        break;
                    qint64 written = 0;
                    while (written < count) {
                        const auto chunk = copy.write(buffer.constData() + written, count - written);
                        if (chunk <= 0) {
                            error = copy.errorString();
                            break;
                        }
                        written += chunk;
                    }
                    if (!error.isEmpty())
                        break;
                    digest.addData(QByteArrayView(buffer.constData(), count));
                    copied += count;
                    if (copied >= checkpointAt) {
                        jobs->checkpoint(bytes(jobId), item, claim.token, copyCheckpoint(copied));
                        checkpointAt = copied + 64 * 1024 * 1024;
                    }
                    reportProgress(name, input.size() > 0 ? double(copied) / input.size() : 0);
                }
                if (cancelled->load())
                    return;
                if (error.isEmpty() && (copied != input.size()
                    || QFileInfo(source.fileName()).lastModified() != input.lastModified())) {
                    error = tr("The source changed while it was being copied. Start a new import for the changed file.");
                    canRetry = false;
                }
                if (error.isEmpty() && !copy.flush())
                    error = copy.errorString();
                if (error.isEmpty())
                    error = iiSocietyContainer::ModelClassifier::validateSafetensors(copy.fileName());
                if (error.isEmpty() && !drive->isValid())
                    error = tr("The Society drive changed while the model was being copied.");
                if (error.isEmpty()) {
                    const double fraction = std::clamp((completed + 0.98) / sources.size(), 0.0, 1.0);
                    QMetaObject::invokeMethod(this, [this, name, fraction] {
                        m_progress = fraction;
                        m_status = tr("Saving %1 and its metadata…").arg(name);
                        emit stateChanged();
                    }, Qt::QueuedConnection);
                    const auto classification = iiSocietyContainer::ModelClassifier::classify(content ? copy.fileName() : input.absoluteFilePath(), name);
                    const auto category = store->categoryPath(classification.type, &error);
                    iiSocietyContainer::detail::AssetObject::ImportFailure publicationFailure{};
                    const auto waiting = [this, name] {
                        QMetaObject::invokeMethod(this, [this, name] {
                            m_status = tr("Waiting for another container operation before saving %1…").arg(name);
                            emit stateChanged();
                        }, Qt::QueuedConnection);
                    };
                    const auto destination = category.isEmpty() ? QString()
                        : iiSocietyContainer::detail::AssetObject::publishImport(copy, content ? QString() : input.absoluteFilePath(),
                                                                                 category, name, cancelled, &error,
                                                                                 &publicationFailure, waiting, catalogReserved);
                    publicationBlocked = publicationFailure == iiSocietyContainer::detail::AssetObject::ImportFailure::LockTimeout;
                    if (destination.isEmpty()) {
                        if (error.isEmpty() && !cancelled->load())
                            error = tr("Could not save a complete model without replacing an existing item: %1").arg(copy.errorString());
                    }
                    else {
                        ++result->copied;
                        result->lastCopiedPath = destination;
                        jobs->checkpoint(bytes(jobId), item, claim.token, publishedCheckpoint(destination, digest.result().toHex()));
                        published = true;
                        canRetry = true;
                        if (!recordObject(destination, name, &error)) {
                            if (!cancelled->load()) result->errors.append(tr("%1 was saved; its managed index needs retry: %2").arg(name, error));
                            return;
                        }
                        result->paths.append(destination);
                    }
                }
                if (!error.isEmpty())
                    result->errors.append(tr("%1: %2").arg(name, error));
              } catch (...) { callbackFailure = std::current_exception(); }
            }, cancelled);
            if (callbackFailure) std::rethrow_exception(callbackFailure);
            if (!readError.isEmpty() && !cancelled->load())
                result->errors.append(tr("%1: %2").arg(entry.name, readError));
            if (!published && !consumed && readError.isEmpty() && !cancelled->load())
                result->errors.append(tr("%1: the provider did not return a file.").arg(entry.name));
            if (cancelled->load()) {
                ii::job::CancelledItem outcome{item, claim.token};
                if (result->paths.size() > pathsBefore) {
                    outcome.state = ii::job::ItemState::succeeded;
                    outcome.artifact = bytes(result->paths.last());
                } else if (published) {
                    outcome.state = ii::job::ItemState::failed;
                    outcome.error = bytes(tr("The model was saved, but registration was interrupted."));
                    outcome.retryable = true;
                }
                jobs->cancelAndFinish(bytes(jobId), outcome);
                return;
            } else if (result->errors.size() > errorsBefore) {
                jobs->fail(bytes(jobId), item, claim.token, bytes(result->errors.last()), canRetry);
            } else if (result->paths.size() > pathsBefore) {
                jobs->succeed(bytes(jobId), item, claim.token, bytes(result->paths.last()));
            } else if (duplicate) {
                jobs->skip(bytes(jobId), item, claim.token, "duplicate input already processed");
            } else {
                jobs->fail(bytes(jobId), item, claim.token, "no result was recorded", canRetry);
            }
            ++completed;
            jobs->progress(bytes(jobId), "importing models", std::uint64_t(completed),
                           std::uint64_t(sources.size()), "items");
        }
        if (cancelled->load()) jobs->cancelAndFinish(bytes(jobId));
        else jobs->finish(bytes(jobId));
      } catch (const std::exception &failure) {
        result->errors.append(tr("The job could not be completed: %1").arg(QString::fromUtf8(failure.what())));
      }
    });
    connect(m_worker, &QThread::finished, this, [this, root, result, cancelled, jobId] {
        m_worker->deleteLater();
        m_worker = nullptr;
        m_cancelled.reset();
        m_error = result->errors.join('\n');
        if (cancelled->load())
            m_status = tr("Import cancelled. %1 model(s) copied to Models.").arg(result->copied);
        else if (result->copied == 0 && result->existing > 0)
            m_status = tr("%1 model(s) already in Models.").arg(result->existing);
        else if (result->copied == 1)
            m_status = tr("Imported %1 to Models.").arg(QFileInfo(result->lastCopiedPath).fileName());
        else
            m_status = tr("%1 model(s) imported to Models.").arg(result->copied);
        if (!cancelled->load() && m_error.isEmpty())
            m_progress = 1;
        emit stateChanged();
        emit finished(root, result->paths);
        if (m_historySelection == jobId) selectJob(jobId); else refreshJobs();
        if (m_needsOrganization) QTimer::singleShot(0, this, [this] { organizeModels(); });
    });
    m_worker->start();
    emit stateChanged();
    return true;
}

void ModelImportService::cancel()
{
    m_recordingCancellation.request_stop();
    if (m_cancelled)
        m_cancelled->store(true);
    if (m_organizationCancelled) m_organizationCancelled->store(true);
    m_organizationCancellation.request_stop();
}

void ModelImportService::yieldOrganization()
{
    if (!m_organizationWorker) return;
    m_needsOrganization = true;
    if (m_organizationCancelled) m_organizationCancelled->store(true);
    m_organizationCancellation.request_stop();
}
