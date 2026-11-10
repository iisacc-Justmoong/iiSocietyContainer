#pragma once
#include <iiSocietyContainerExport.h>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>

namespace iiSocietyContainer {
// Container-owned asynchronous operations. The owner must live on one Qt
// event-loop thread; workers and durable records are independent of views.
class IISOCIETYCONTAINER_EXPORT FileJobService final : public QObject {
    Q_OBJECT
public:
    enum class Kind { Duplicate, Rename, Trash, Remove, Restore, Move, CreateDirectory, Copy };
    explicit FileJobService(QString jobDirectory = {}, QObject *parent = nullptr);
    QString submit(Kind kind, const QString &container, const QString &source,
                   const QString &argument = {}, QString *error = nullptr);
    void inspect(const QString &ticket, const QString &jobId);
signals:
    // The initial submit is only queued; recorded means a durable job exists.
    void recorded(QString jobId);
    // A nonempty path with an error means an effect occurred only partially.
    void finished(QString jobId, QString path, QString error);
    void inspected(QString ticket, QVariantMap state, QString error);
private:
    QString m_jobDirectory;
    QHash<QString, QString> m_activePaths;
};
}
