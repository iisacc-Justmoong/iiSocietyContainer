#pragma once
#include "StorageMap.h"
#include <QAbstractListModel>
#include <QTimer>
#include <QUrl>
#include <atomic>
#include <memory>

namespace iiSocietyContainer {
// Namespace-backed directory rows. FolderListModel-compatible roles let a GUI
// retain names, sizes and previews before any original payload is downloaded.
class IISOCIETYCONTAINER_EXPORT StorageDirectoryModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QUrl folder READ folder WRITE setFolder NOTIFY folderChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadingChanged)
    Q_PROPERTY(bool showDirs MEMBER m_showDirs NOTIFY optionsChanged)
    Q_PROPERTY(bool showFiles MEMBER m_showFiles NOTIFY optionsChanged)
    Q_PROPERTY(bool showHidden MEMBER m_showHidden NOTIFY optionsChanged)
    Q_PROPERTY(bool showDotAndDotDot MEMBER m_showDotAndDotDot NOTIFY optionsChanged)
    Q_PROPERTY(bool showDirsFirst MEMBER m_showDirsFirst NOTIFY optionsChanged)
    Q_PROPERTY(bool caseSensitive MEMBER m_caseSensitive NOTIFY optionsChanged)
    Q_PROPERTY(bool sortCaseSensitive MEMBER m_sortCaseSensitive NOTIFY optionsChanged)
    Q_PROPERTY(bool sortReversed MEMBER m_sortReversed NOTIFY optionsChanged)
    Q_PROPERTY(SortField sortField MEMBER m_sortField NOTIFY optionsChanged)
    Q_PROPERTY(QStringList nameFilters MEMBER m_nameFilters NOTIFY optionsChanged)
public:
    enum Status { Null, Ready, Loading }; Q_ENUM(Status)
    enum SortField { Name, Time, Size, Type, Unsorted }; Q_ENUM(SortField)
    explicit StorageDirectoryModel(QObject *parent = nullptr);
    ~StorageDirectoryModel() override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QUrl folder() const { return m_folder; }
    void setFolder(const QUrl &folder);
    Status status() const { return m_status; }
    bool downloading() const { return m_downloading; }
    Q_INVOKABLE QVariant get(int index, const QString &role) const;
    Q_INVOKABLE bool isFolder(int index) const;
    Q_INVOKABLE void activate(int index);
    // For a model package, materialize its catalogued files before opening it.
    Q_INVOKABLE void openPath(const QString &path, bool materializeDirectory = false);
    Q_INVOKABLE void refresh();
signals:
    // One publication boundary per changed snapshot; polling alone emits neither.
    void contentsAboutToChange();
    void contentsChanged();
    void folderChanged();
    void countChanged();
    void statusChanged();
    void optionsChanged();
    void downloadingChanged();
    void activated(QString path, bool directory);
    void downloadFailed(QString error);
private:
    void applyRows(const QList<QVariantMap> &rows);
    void checkRequest();
    QUrl m_folder;
    Status m_status = Null;
    SortField m_sortField = Name;
    bool m_showDirs = true, m_showFiles = true, m_showHidden = false, m_showDotAndDotDot = false;
    bool m_showDirsFirst = true, m_caseSensitive = false, m_sortCaseSensitive = false, m_sortReversed = false;
    QStringList m_nameFilters;
    QList<QVariantMap> m_rows;
    std::optional<SocietyDrive> m_drive;
    QJsonArray m_catalog;
    QString m_catalogStamp;
    QString m_request, m_requestedPath;
    std::optional<SocietyDrive> m_requestDrive;
    bool m_running = false, m_pending = false, m_downloading = false, m_checkingRequest = false;
    bool m_requestedDirectory = false;
    quint64 m_revision = 0, m_requestRevision = 0, m_optionsRevision = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QTimer m_poll, m_requestPoll;
};
}
