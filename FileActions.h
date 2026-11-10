#pragma once
#include "StorageDirectoryModel.h"
#include "iiSocietyContainerExport.h"
#include <QHash>
#include <QUrl>

namespace iiSocietyContainer {
// GUI-thread adapter; payload materialization and mutations run asynchronously.
// Used by any Society consumer without putting filesystem logic in its QML.
class IISOCIETY_SHARING_EXPORT FileActions : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(bool editable READ editable NOTIFY eligibilityChanged)
    Q_PROPERTY(bool inDeleted READ inDeleted NOTIFY eligibilityChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString errorString READ errorString NOTIFY changed)
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY clipboardChanged)
    Q_PROPERTY(bool canShare READ sharingAvailable CONSTANT)
public:
    explicit FileActions(QObject *parent = nullptr);
    ~FileActions() override;
    QString path() const { return m_path; }
    void setPath(const QString &path);
    bool editable() const;
    bool inDeleted() const;
    bool busy() const { return !m_operations.isEmpty(); }
    int activeCount() const { return m_operations.size(); }
    Q_INVOKABLE bool busyForPath(const QString &path) const;
    QString status() const { return m_status; }
    QString errorString() const { return m_error; }
    bool canPaste() const;
    Q_INVOKABLE void copy();
    Q_INVOKABLE void copyPath();
    Q_INVOKABLE void paste(const QString &folder);
    Q_INVOKABLE void duplicate();
    Q_INVOKABLE void rename(const QString &name);
    Q_INVOKABLE void trash();
    Q_INVOKABLE void remove();
    Q_INVOKABLE void removePath(const QString &path);
    Q_INVOKABLE void restore();
    Q_INVOKABLE void restoreTo(const QUrl &folder);
    Q_INVOKABLE void share();
    static bool sharingAvailable();
    static bool shareFiles(const QStringList &paths, QString *error = nullptr);
    static void copyFiles(const QStringList &paths);
signals:
    void pathChanged();
    void eligibilityChanged();
    void changed();
    void clipboardChanged();
    void completed(QString path);
    /// A native change succeeded but a subsequent managed-index update failed.
    void partiallyCompleted(QString path, QString error);
    void failed(QString error);
    // Identifies the request even when another item was selected before it finished.
    void operationFinished(QString source, QString path, QString error);
private:
    enum Operation { Clipboard, Share, Duplicate, Rename, Trash, Remove, Restore, RestoreTo, Paste };
    class Request;
    void begin(Operation operation, const QString &source, const QString &argument = {}, const QStringList &sources = {});
    void finish(quint64 id, Operation operation, const QString &source, const QString &path, const QString &error);
    QString m_path, m_status, m_error;
    QHash<quint64, QString> m_operations;
    quint64 m_nextOperation = 0;
    quint64 m_pathEpoch = 0;
    bool m_editable = false;
    bool m_inDeleted = false;
};
}
