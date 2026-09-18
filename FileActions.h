#pragma once
#include "StorageDirectoryModel.h"
#include <QThreadPool>

#if defined(IISOCIETYCONTAINER_GUI_BUILD)
#define IISOCIETY_GUI_EXPORT Q_DECL_EXPORT
#else
#define IISOCIETY_GUI_EXPORT Q_DECL_IMPORT
#endif
namespace iiSocietyContainer {
// GUI-thread adapter; payload materialization and mutations run asynchronously.
// Used by any Society consumer without putting filesystem logic in its QML.
class IISOCIETY_GUI_EXPORT FileActions : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(bool editable READ editable NOTIFY pathChanged)
    Q_PROPERTY(bool inDeleted READ inDeleted NOTIFY pathChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
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
    bool busy() const { return m_busy; }
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
    Q_INVOKABLE void share();
    static bool sharingAvailable();
    static bool shareFiles(const QStringList &paths, QString *error = nullptr);
    static void copyFiles(const QStringList &paths);
signals:
    void pathChanged();
    void changed();
    void clipboardChanged();
    void completed(QString path);
    void failed(QString error);
private:
    enum Operation { Clipboard, Share, Duplicate, Rename, Trash, Remove, Paste };
    void begin(Operation operation, const QString &source, const QString &argument = {});
    void execute();
    void finish(const QString &path, const QString &error);
    QString m_path, m_source, m_argument, m_status, m_error;
    QStringList m_pasteSources;
    Operation m_operation = Clipboard;
    bool m_busy = false;
    StorageDirectoryModel m_download;
    QThreadPool m_worker;
};
}
