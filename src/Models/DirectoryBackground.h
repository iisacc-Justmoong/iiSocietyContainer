#pragma once
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QPointer>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <type_traits>
#include <utility>

namespace iiSocietyContainer::detail {
// UI-thread models share an application-owned pool. Other QObject threads use
// an owner-local pool, so creation never blocks on a different event loop.
inline QThreadPool *directoryPool(QObject *owner)
{
    auto *app=QCoreApplication::instance();
    if (app && app->thread()==QThread::currentThread()) {
        static QPointer<QThreadPool> shared;
        if (!shared) { shared=new QThreadPool(app); shared->setMaxThreadCount(4); }
        return shared;
    }
    const auto name=QStringLiteral("iiSocietyContainer.directoryPool");
    auto *local=owner->findChild<QThreadPool *>(name,Qt::FindDirectChildrenOnly);
    if (!local) { local=new QThreadPool(owner); local->setObjectName(name); local->setMaxThreadCount(2); }
    return local;
}

template<class Work,class Done> void directoryBackground(QObject *owner,Work work,Done done)
{
    using Result=std::invoke_result_t<Work>;
    auto *watcher=new QFutureWatcher<Result>(owner);
    QObject::connect(watcher,&QFutureWatcher<Result>::finished,owner,[watcher,done=std::move(done)]() mutable {
        auto result=watcher->result(); watcher->deleteLater(); done(std::move(result));
    });
    watcher->setFuture(QtConcurrent::run(directoryPool(owner),std::move(work)));
}
}
