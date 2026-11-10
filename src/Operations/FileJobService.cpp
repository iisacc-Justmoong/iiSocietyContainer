#include "FileJobService.h"
#include "../Models/ContainerJobStorage.h"
#include "../../FileOperations.h"
#include <Job.h>
#include <TemporaryLease.h>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QPromise>
#include <QScopeGuard>
#include <QSysInfo>
#include <QThreadPool>
#include <QUuid>
#include <memory>

namespace iiSocietyContainer {
namespace {
struct Event { bool recorded = false; QString path, error; };
struct Inspection { QVariantMap state; QString error; };
QThreadPool *pool() {
    static QPointer<QThreadPool> instance;
    if (!instance) { instance = new QThreadPool(QCoreApplication::instance()); instance->setMaxThreadCount(4); }
    return instance;
}
QString operationName(FileJobService::Kind kind) {
    switch (kind) {
    case FileJobService::Kind::Duplicate: return "duplicate";
    case FileJobService::Kind::Rename: return "rename";
    case FileJobService::Kind::Trash: return "trash";
    case FileJobService::Kind::Remove: return "remove";
    case FileJobService::Kind::Restore: return "restore";
    case FileJobService::Kind::Move: return "move";
    case FileJobService::Kind::CreateDirectory: return "create-directory";
    case FileJobService::Kind::Copy: return "copy";
    }
    return {};
}
bool validRelative(const QString &path) {
    if (path.isEmpty() || path.size()>4096 || QDir::isAbsolutePath(path)
        || path.contains('\\') || path.contains(QChar::Null)) return false;
    for (const auto &component:path.split('/'))
        if (component.isEmpty() || component=="." || component=="..") return false;
    return true;
}
QString normalizedKey(QString result) {
    result=QDir::cleanPath(result);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    result=result.normalized(QString::NormalizationForm_D).toCaseFolded();
#endif
    return result;
}
std::string bytes(const QString &value) { return value.toUtf8().toStdString(); }
QString name(ii::job::JobState state) {
    using enum ii::job::JobState;
    switch (state) {
    case waiting: return "waiting"; case waitingNode: return "waitingNode";
    case waitingPermission: return "waitingPermission"; case running: return "running";
    case partial: return "partial"; case failed: return "failed";
    case cancelRequested: return "cancelRequested"; case cancelled: return "cancelled";
    case succeeded: return "succeeded"; case attention: return "attention";
    }
    return "unknown";
}
}
FileJobService::FileJobService(QString jobDirectory,QObject *parent)
    : QObject(parent),m_jobDirectory(std::move(jobDirectory)) {}

QString FileJobService::submit(Kind kind,const QString &container,const QString &source,
                               const QString &argument,QString *error) {
    if (error) error->clear();
    auto reject=[&](const QString &message){if(error)*error=message;return QString{};};
    const auto operation=operationName(kind);
    if (operation.isEmpty() || !QDir::isAbsolutePath(container) || container.size()>4096
        || (kind==Kind::Copy ? (!QDir::isAbsolutePath(source) || source.size()>4096 || source.contains(QChar::Null)
                                || QDir::cleanPath(source)!=source) : !validRelative(source))
        || argument.size()>4096 || argument.contains(QChar::Null))
        return reject(tr("Invalid file operation request."));
    if ((kind==Kind::Move || kind==Kind::Copy) && !validRelative(argument)) return reject(tr("Invalid destination."));
    if ((kind==Kind::Rename || kind==Kind::CreateDirectory) && (argument.isEmpty() || argument.contains('/') || argument.contains('\\')))
        return reject(tr("Enter a valid name."));
    const auto path=normalizedKey(kind==Kind::Copy ? source : QDir::cleanPath(container)+"/"+source);
    for (auto it=m_activePaths.cbegin();it!=m_activePaths.cend();++it)
        if (path==it.value() || path.startsWith(it.value()+"/") || it.value().startsWith(path+"/"))
            return reject(tr("An operation on this item is already in progress."));
    if (m_activePaths.size()>=128) return reject(tr("Too many file operations are pending."));
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_activePaths.insert(id,path);
    auto promise=std::make_shared<QPromise<Event>>(); promise->start();
    auto *watcher=new QFutureWatcher<Event>(this);
    connect(watcher,&QFutureWatcher<Event>::resultReadyAt,this,[this,watcher,id](int index){
        const auto event=watcher->resultAt(index);
        if(event.recorded) { emit recorded(id); return; }
        m_activePaths.remove(id); emit finished(id,event.path,event.error);
    });
    connect(watcher,&QFutureWatcher<Event>::finished,watcher,&QObject::deleteLater);
    watcher->setFuture(promise->future());
    const auto jobDirectory=m_jobDirectory;
    pool()->start([promise,id,operation,kind,container,source,argument,jobDirectory]{
        auto complete=qScopeGuard([&]{promise->finish();});
        QString resultPath;
        std::unique_ptr<ii::job::Store> store;
        std::unique_ptr<iiFileProvider::TemporaryLease> lease;
        bool recorded=false, effectStarted=false;
        try {
            QString failure;
            const auto drive=SocietyDrive::open(container,&failure);
            if (!drive || !drive->isReady()) throw std::runtime_error(bytes(failure.isEmpty()?QStringLiteral("The Society drive is unavailable."):failure));
            const auto driveId=drive->storageKind()==SocietyDrive::StorageKind::LogicalDisk ? drive->storageIdentifier() : drive->identifier();
            if (driveId.isEmpty()) throw std::runtime_error("The Society drive has no stable identity.");
            const auto sourcePath=kind==Kind::Copy ? source : drive->resolvePath(source);
            if (sourcePath.isEmpty()) throw std::runtime_error("The selected path is unavailable.");
            auto resolvedArgument=argument;
            if (kind==Kind::Move || kind==Kind::Copy) {
                resolvedArgument=drive->resolvePath(argument);
                if (resolvedArgument.isEmpty()) throw std::runtime_error("The destination is unavailable.");
            }
            const auto storagePath=jobDirectory.isEmpty() ? ContainerJobStorage::defaultDirectory() : std::filesystem::path(jobDirectory.toStdU16String());
            ContainerJobStorage storage(storagePath); store=storage.open(true);
            lease=storage.tryLease(bytes(id),true);
            if (!lease || !lease->locked()) throw std::runtime_error("Could not acquire the file job lease.");
            const auto node=bytes(QStringLiteral("local:")+QSysInfo::machineHostName());
            store->create({bytes(id),"file-"+bytes(operation),"local-user:unverified",bytes(driveId),"",node,false},
                          {{"item",bytes(source),"","",bytes(QFileInfo(source).fileName())}});
            recorded=true; promise->addResult(Event{true,{},{}});
            store->begin(bytes(id),node,bytes(operation));
            const auto claim=store->claim(bytes(id),"item");
            const auto descriptor=QJsonDocument(QJsonObject{{"operation",operation},{"source",source},{"argument",argument}}).toJson(QJsonDocument::Compact);
            store->checkpoint(bytes(id),"item",claim.token,descriptor.toStdString());
            FileOperations::Result result;
            effectStarted=true;
            if(kind==Kind::CreateDirectory) result=FileOperations(*drive).createDirectory(sourcePath,argument);
            else {
                const auto action=kind==Kind::Copy?FileOperations::Action::Copy:kind==Kind::Duplicate?FileOperations::Action::Duplicate:kind==Kind::Rename?FileOperations::Action::Rename:
                    kind==Kind::Trash?FileOperations::Action::Trash:kind==Kind::Remove?FileOperations::Action::Remove:
                    kind==Kind::Restore?FileOperations::Action::Restore:FileOperations::Action::Move;
                result=FileOperations(*drive).perform(action,sourcePath,resolvedArgument);
            }
            resultPath=result.path.isEmpty()?QString{}:drive->relativePath(result.path);
            if (result) store->succeed(bytes(id),"item",claim.token,bytes(resultPath));
            else {
                if (!resultPath.isEmpty()) {
                    const auto partial=QJsonDocument(QJsonObject{{"operation",operation},{"source",source},{"argument",argument},
                                                               {"result",resultPath}}).toJson(QJsonDocument::Compact);
                    store->checkpoint(bytes(id),"item",claim.token,partial.toStdString());
                }
                store->fail(bytes(id),"item",claim.token,bytes(result.error),false);
            }
            store->finish(bytes(id));
            // Publish completion only after the durable writer and its lease
            // have released their handles; observers may immediately reopen it.
            store.reset(); lease.reset();
            promise->addResult(Event{false,resultPath,result.error});
        } catch (const std::exception &failure) {
            if (recorded && store) {
                try { if (auto job=store->job(bytes(id));job && (job->state==ii::job::JobState::running || job->state==ii::job::JobState::cancelRequested)) store->recoverInterrupted(bytes(id)); }
                catch (...) {}
            }
            const auto prefix=effectStarted?QStringLiteral("The file operation may have completed; inspect its result before retrying: "):QString();
            promise->addResult(Event{false,resultPath,prefix+QString::fromUtf8(failure.what())});
        } catch (...) {
            if (recorded && store) {
                try { if (auto job=store->job(bytes(id));job && job->state==ii::job::JobState::running) store->recoverInterrupted(bytes(id)); }
                catch (...) {}
            }
            promise->addResult(Event{false,resultPath,effectStarted?QStringLiteral("The file operation may have completed; inspect it before retrying.")
                                                                  :QStringLiteral("The file operation failed unexpectedly.")});
        }
    });
    return id;
}

void FileJobService::inspect(const QString &ticket,const QString &jobId) {
    auto promise=std::make_shared<QPromise<Inspection>>(); promise->start();
    auto *watcher=new QFutureWatcher<Inspection>(this);
    connect(watcher,&QFutureWatcher<Inspection>::finished,this,[this,watcher,ticket] {
        const auto result=watcher->result(); watcher->deleteLater();
        emit inspected(ticket,result.state,result.error);
    });
    watcher->setFuture(promise->future());
    const auto jobDirectory=m_jobDirectory;
    pool()->start([promise,jobId,jobDirectory] {
        auto complete=qScopeGuard([&]{promise->finish();});
        try {
            if (QUuid(jobId).isNull()) throw std::runtime_error("Invalid file job identifier.");
            const auto path=jobDirectory.isEmpty()?ContainerJobStorage::defaultDirectory():std::filesystem::path(jobDirectory.toStdU16String());
            ContainerJobStorage storage(path); auto store=storage.open(false);
            if (!store) throw std::runtime_error("The file job was not found.");
            auto job=store->job(bytes(jobId));
            if (!job || !job->kind.starts_with("file-")) throw std::runtime_error("The file job was not found.");
            if (job->state==ii::job::JobState::running || job->state==ii::job::JobState::cancelRequested) {
                auto lease=storage.tryLease(bytes(jobId),false);
                if (lease && lease->locked()) { store->recoverInterrupted(bytes(jobId)); job=store->job(bytes(jobId)); }
            }
            auto item=store->item(bytes(jobId),"item"); QString pathResult, itemError;
            if (item) {
                pathResult=QString::fromStdString(item->artifact);
                if (pathResult.isEmpty())
                    pathResult=QJsonDocument::fromJson(QByteArray::fromStdString(item->checkpoint)).object().value("result").toString();
                itemError=QString::fromStdString(item->error);
            }
            promise->addResult(Inspection{{{"jobId",jobId},{"state",name(job->state)},{"kind",QString::fromStdString(job->kind)},
                {"containerId",QString::fromStdString(job->resource)}, {"stage",QString::fromStdString(job->stage)},
                {"path",pathResult},{"error",itemError},
                {"unknown",qulonglong(job->unknown)}},{}});
        } catch (const std::exception &error) { promise->addResult(Inspection{{},QString::fromUtf8(error.what())}); }
        catch (...) { promise->addResult(Inspection{{},QStringLiteral("Could not inspect the file job.")}); }
    });
}
}
