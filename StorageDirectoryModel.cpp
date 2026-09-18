#include "StorageDirectoryModel.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <type_traits>

namespace iiSocietyContainer {
namespace {
template<class Work, class Done> void background(QObject *owner, Work work, Done done) {
    auto result = std::make_shared<std::invoke_result_t<Work>>();
    auto *thread = QThread::create([result, work = std::move(work)] { *result = work(); });
    QObject::connect(thread, &QThread::finished, owner, [result, done = std::move(done)] { done(std::move(*result)); });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
std::optional<SocietyDrive> containingDrive(QString path) {
    while (!path.isEmpty() && path != QDir::rootPath()) {
        if (QFileInfo::exists(QDir(path).filePath(".society-drive.json"))) return SocietyDrive::open(path);
        path = QFileInfo(path).absolutePath();
    }
    return {};
}
struct Options {
    QUrl folder;
    bool dirs, files, hidden, dirsFirst, caseSensitive, sortCaseSensitive, reversed;
    StorageDirectoryModel::SortField sort;
    QStringList filters;
};
struct Listing {
    QList<QVariantMap> rows;
    std::optional<SocietyDrive> drive;
    QJsonArray catalog;
    QString stamp;
};
Listing list(const Options &o, const QJsonArray &cached, const QString &cachedStamp,
             const std::shared_ptr<std::atomic_bool> &cancel) {
    Listing result;
    if (!o.folder.isLocalFile() || o.folder.toLocalFile().isEmpty()) return result;
    const auto folder = QDir::cleanPath(o.folder.toLocalFile());
    result.drive = containingDrive(folder);
    QMap<QString, QVariantMap> paths;
    for (const auto &info : QDir(folder).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
        if (cancel->load()) return {};
        const auto path = info.absoluteFilePath();
        paths.insert(path, {{"fileName", info.fileName()}, {"filePath", path}, {"fileUrl", QUrl::fromLocalFile(path)},
            {"fileSuffix", info.suffix()}, {"fileIsDir", info.isDir()}, {"fileSize", info.size()},
            {"fileModified", info.lastModified()}, {"filePreviewUrl", QUrl::fromLocalFile(path)}, {"fileResident", true}});
    }
    if (result.drive) {
        const auto root = result.drive->rootPath();
        const auto relativeFolder = QDir(root).relativeFilePath(folder);
        const QFileInfo catalog(QDir(root).filePath(".society-sync/catalog.json"));
        result.stamp = root + '\n' + result.drive->identifier() + '\n' + QString::number(catalog.size())
            + '\n' + QString::number(catalog.lastModified().toMSecsSinceEpoch())
            + '\n' + QString::number(catalog.metadataChangeTime().toMSecsSinceEpoch());
        StorageMap map(*result.drive);
        const bool authority = map.isLocalAuthority();
        result.catalog = result.stamp == cachedStamp ? cached : map.objects();
        for (const auto &value : result.catalog) {
            if (cancel->load()) return {};
            const auto e = value.toObject(); const auto key = e.value("path").toString();
            // Select immediate children before touching any payload path. A
            // Models view must not stat every photo or model in the namespace.
            const auto relative = StorageMap::physicalPath(key);
            if (relative.isEmpty() || relative.left(relative.lastIndexOf('/')) != relativeFolder) continue;
            const auto path = map.localPath(key); if (path.isEmpty()) continue;
            // On the local host, the directory already is authoritative. An old
            // index cannot resurrect a removed path or hide a newly restored file.
            if (authority && (!paths.contains(path) || e.value("kind") == "deleted")) continue;
            if (e.value("kind") == "deleted") { paths.remove(path); continue; }
            const bool directory = e.value("kind") == "directory";
            if (!directory && e.value("kind") != "file") continue;
            auto row = paths.value(path);
            if (authority && directory != row.value("fileIsDir").toBool()) continue;
            const bool local = authority || directory || map.isResident(e);
            const auto preview = map.previewPath(e);
            row.insert("fileName", key.section('/', -1)); row.insert("filePath", path);
            row.insert("fileUrl", QUrl::fromLocalFile(path)); row.insert("fileSuffix", QFileInfo(path).suffix());
            row.insert("fileIsDir", directory);
            if (!authority) row.insert("fileSize", e.value("size").toString().toLongLong());
            row.insert("fileResident", local);
            row.insert("filePreviewUrl", !preview.isEmpty() ? QUrl::fromLocalFile(preview) : local ? QUrl::fromLocalFile(path) : QUrl());
            if (!row.contains("fileModified")) row.insert("fileModified", QDateTime());
            paths.insert(path, row);
        }
    }
    QList<QRegularExpression> filters;
    for (const auto &filter : o.filters)
        filters.append(QRegularExpression(QRegularExpression::wildcardToRegularExpression(filter),
            o.caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption));
    for (const auto &row : paths) {
        if (cancel->load()) return {};
        const auto name = row.value("fileName").toString(); const bool directory = row.value("fileIsDir").toBool();
        if ((!o.hidden && name.startsWith('.')) || (directory ? !o.dirs : !o.files)) continue;
        bool match = filters.isEmpty() || directory;
        for (const auto &filter : filters) match |= filter.match(name).hasMatch();
        if (match) result.rows.append(row);
    }
    std::sort(result.rows.begin(), result.rows.end(), [&o](const auto &a, const auto &b) {
        if (o.dirsFirst && a.value("fileIsDir") != b.value("fileIsDir")) return a.value("fileIsDir").toBool();
        int comparison;
        if (o.sort == StorageDirectoryModel::Time && a.value("fileModified") != b.value("fileModified")) comparison = a.value("fileModified").toDateTime() > b.value("fileModified").toDateTime() ? -1 : 1;
        else if (o.sort == StorageDirectoryModel::Size && a.value("fileSize") != b.value("fileSize")) comparison = a.value("fileSize").toLongLong() > b.value("fileSize").toLongLong() ? -1 : 1;
        else comparison = a.value("fileName").toString().compare(b.value("fileName").toString(), o.sortCaseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
        return o.reversed ? comparison > 0 : comparison < 0;
    });
    return result;
}
struct OpenResult {
    std::optional<SocietyDrive> drive;
    QString request, error;
    bool ready = false, directory = false;
};
}
StorageDirectoryModel::StorageDirectoryModel(QObject *parent) : QAbstractListModel(parent) {
    m_poll.setInterval(1000); m_requestPoll.setInterval(500);
    connect(&m_poll, &QTimer::timeout, this, &StorageDirectoryModel::refresh);
    connect(&m_requestPoll, &QTimer::timeout, this, &StorageDirectoryModel::checkRequest);
    connect(this, &StorageDirectoryModel::optionsChanged, this, [this] { ++m_optionsRevision; refresh(); });
}
StorageDirectoryModel::~StorageDirectoryModel() { if (m_cancel) m_cancel->store(true); }
int StorageDirectoryModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_rows.size(); }
QHash<int, QByteArray> StorageDirectoryModel::roleNames() const {
    QHash<int, QByteArray> roles; int role = Qt::UserRole + 1;
    for (const auto *name : {"fileName", "filePath", "fileUrl", "fileSuffix", "fileIsDir", "fileSize", "fileModified", "filePreviewUrl", "fileResident"}) roles.insert(role++, name);
    return roles;
}
QVariant StorageDirectoryModel::data(const QModelIndex &index, int role) const { return get(index.row(), QString::fromLatin1(roleNames().value(role))); }
QVariant StorageDirectoryModel::get(int index, const QString &role) const { return index >= 0 && index < m_rows.size() ? m_rows[index].value(role) : QVariant(); }
bool StorageDirectoryModel::isFolder(int index) const { return get(index, "fileIsDir").toBool(); }
void StorageDirectoryModel::setFolder(const QUrl &folder) {
    if (m_folder == folder) return;
    if (m_cancel) m_cancel->store(true);
    ++m_revision; ++m_requestRevision; m_running = m_pending = m_checkingRequest = false;
    m_folder = folder; m_drive.reset(); m_catalog = {}; m_catalogStamp.clear();
    m_request.clear(); m_requestDrive.reset(); m_requestPoll.stop();
    if (m_downloading) { m_downloading = false; emit downloadingChanged(); }
    if (!m_rows.isEmpty()) { beginResetModel(); m_rows.clear(); endResetModel(); emit countChanged(); }
    m_status = folder.isLocalFile() && !folder.toLocalFile().isEmpty() ? Loading : Null;
    emit folderChanged(); emit statusChanged();
    if (m_status == Null) { m_poll.stop(); return; }
    m_poll.start(); QTimer::singleShot(0, this, &StorageDirectoryModel::refresh);
}
void StorageDirectoryModel::activate(int index) {
    if (index < 0 || index >= m_rows.size()) return;
    const auto row = m_rows[index]; const auto path = row.value("filePath").toString();
    if (row.value("fileIsDir").toBool() || row.value("fileResident").toBool()) { emit activated(path, row.value("fileIsDir").toBool()); return; }
    openPath(path);
}
void StorageDirectoryModel::openPath(const QString &path, bool materializeDirectory) {
    if (m_downloading && path == m_requestedPath) return;
    const auto revision = ++m_requestRevision;
    const auto oldId = m_request; const auto oldDrive = m_requestDrive;
    m_request.clear(); m_requestDrive.reset(); m_requestPoll.stop(); m_checkingRequest = false;
    m_requestedPath = path; m_downloading = true; emit downloadingChanged();
    background(this, [path, materializeDirectory, oldId, oldDrive] {
        OpenResult r;
        if (oldDrive && !oldId.isEmpty()) StorageMap(*oldDrive).cancel(oldId);
        if (!QFileInfo(path).isAbsolute()) { r.error = tr("The file is outside the Society container."); return r; }
        r.drive = containingDrive(QFileInfo(path).absolutePath());
        if (!r.drive) { r.error = tr("The Society container is unavailable."); return r; }
        StorageMap map(*r.drive);
        const auto key = StorageMap::logicalPath(QDir(r.drive->rootPath()).relativeFilePath(path));
        const auto safePath = map.localPath(key);
        if (safePath.isEmpty() || safePath != QDir::cleanPath(path)) { r.error = tr("The file is outside the Society container."); return r; }
        r.directory = materializeDirectory || QFileInfo(safePath).isDir();
        const auto keys = materializeDirectory ? map.files(key) : QStringList{key};
        if (!keys.isEmpty() && !map.object(keys.first()).isEmpty()) {
            if (map.available(keys)) r.ready = true;
            else r.request = map.request(keys, &r.error);
        } else if (QFileInfo(safePath).isFile() || (materializeDirectory && QFileInfo(safePath).isDir())) r.ready = true;
        else r.error = tr("The file is no longer in the Society storage map.");
        return r;
    }, [this, revision, path](OpenResult r) {
        if (revision != m_requestRevision) return;
        m_requestDrive = r.drive; m_request = r.request; m_requestedDirectory = r.directory;
        if (!m_request.isEmpty()) { m_requestPoll.start(); return; }
        m_downloading = false; emit downloadingChanged();
        if (r.ready) emit activated(path, r.directory);
        else emit downloadFailed(r.error);
    });
}
void StorageDirectoryModel::checkRequest() {
    if (m_checkingRequest || !m_requestDrive || m_request.isEmpty()) return;
    m_checkingRequest = true;
    const auto revision = m_requestRevision; const auto drive = *m_requestDrive;
    const auto id = m_request, path = m_requestedPath;
    background(this, [drive, id, path] {
        StorageMap map(drive); auto state = map.requestState(id);
        if (state.value("state") == "ready") {
            const auto key = StorageMap::logicalPath(QDir(drive.rootPath()).relativeFilePath(path));
            if (!map.available(map.files(key))) { state["state"] = "failed"; state["error"] = tr("The file changed before it could be opened. Select it again."); }
        }
        return state;
    }, [this, revision, path](QJsonObject state) {
        if (revision != m_requestRevision) return;
        m_checkingRequest = false;
        const auto status = state.value("state").toString();
        if (status != "ready" && status != "failed" && status != "cancelled") return;
        m_request.clear(); m_requestPoll.stop(); m_downloading = false; emit downloadingChanged();
        refresh();
        if (status == "ready") emit activated(path, m_requestedDirectory);
        else emit downloadFailed(state.value("error").toString());
    });
}
void StorageDirectoryModel::applyRows(const QList<QVariantMap> &rows) {
    if (rows == m_rows) return;
    emit contentsAboutToChange();
    const auto previousCount = m_rows.size();
    const auto path = [](const QVariantMap &row) { return row.value("filePath").toString(); };
    QSet<QString> nextPaths, existingPaths;
    for (const auto &row : rows) nextPaths.insert(path(row));
    for (const auto &row : m_rows) existingPaths.insert(path(row));

    // Remove contiguous missing ranges backwards; surviving indexes keep their identity.
    for (int last = m_rows.size() - 1; last >= 0;) {
        if (nextPaths.contains(path(m_rows[last]))) { --last; continue; }
        int first = last;
        while (first > 0 && !nextPaths.contains(path(m_rows[first - 1]))) --first;
        beginRemoveRows({}, first, last);
        m_rows.remove(first, last - first + 1);
        endRemoveRows();
        last = first - 1;
    }
    const auto roles = roleNames();
    for (int target = 0; target < rows.size(); ++target) {
        const auto key = path(rows[target]);
        if (!existingPaths.contains(key)) {
            int last = target;
            while (last + 1 < rows.size() && !existingPaths.contains(path(rows[last + 1]))) ++last;
            beginInsertRows({}, target, last);
            for (int i = target; i <= last; ++i) m_rows.insert(i, rows[i]);
            endInsertRows();
            target = last;
            continue;
        }
        if (path(m_rows[target]) != key) {
            int source = target + 1;
            while (path(m_rows[source]) != key) ++source;
            beginMoveRows({}, source, source, {}, target);
            m_rows.move(source, target);
            endMoveRows();
        }
        QList<int> changedRoles;
        for (auto role = roles.cbegin(); role != roles.cend(); ++role)
            if (m_rows[target].value(QString::fromLatin1(role.value())) != rows[target].value(QString::fromLatin1(role.value())))
                changedRoles.append(role.key());
        if (!changedRoles.isEmpty()) {
            m_rows[target] = rows[target];
            emit dataChanged(index(target), index(target), changedRoles);
        }
    }
    if (previousCount != m_rows.size()) emit countChanged();
    emit contentsChanged();
}
void StorageDirectoryModel::refresh() {
    if (!m_folder.isLocalFile() || m_folder.toLocalFile().isEmpty()) return;
    if (m_running) { m_pending = true; return; }
    m_running = true; m_pending = false;
    const auto revision = m_revision;
    const auto optionsRevision = m_optionsRevision;
    const auto cancel = m_cancel = std::make_shared<std::atomic_bool>(false);
    const Options options{m_folder, m_showDirs, m_showFiles, m_showHidden, m_showDirsFirst, m_caseSensitive, m_sortCaseSensitive, m_sortReversed, m_sortField, m_nameFilters};
    const auto cached = m_catalog; const auto stamp = m_catalogStamp;
    background(this, [options, cached, stamp, cancel] { return list(options, cached, stamp, cancel); },
        [this, revision, optionsRevision](Listing result) {
            if (revision != m_revision) return;
            if (optionsRevision != m_optionsRevision) {
                m_running = false;
                QTimer::singleShot(0, this, &StorageDirectoryModel::refresh);
                return;
            }
            m_running = false; m_drive = std::move(result.drive); m_catalog = std::move(result.catalog); m_catalogStamp = std::move(result.stamp);
            applyRows(result.rows);
            if (m_status != Ready) { m_status = Ready; emit statusChanged(); }
            if (m_pending) QTimer::singleShot(0, this, &StorageDirectoryModel::refresh);
        });
}
}
