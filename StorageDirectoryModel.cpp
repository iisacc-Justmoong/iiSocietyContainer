#include "StorageDirectoryModel.h"
#include "FileOperations.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <type_traits>
#include <filesystem>
#include <mutex>
#include <future>
#include <thread>
#include <QCryptographicHash>
#include <QDataStream>
#include <QSaveFile>
#include <QStandardPaths>

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
    return FileOperations::containingDrive(path);
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
QString snapshotPath(const Options &o) {
    QByteArray key; QDataStream stream(&key, QIODevice::WriteOnly);
    stream << o.folder << o.dirs << o.files << o.hidden << o.dirsFirst << o.caseSensitive
           << o.sortCaseSensitive << o.reversed << int(o.sort) << o.filters;
    return QDir(StorageDirectoryModel::cacheDirectory()).filePath(
        "directories-v1/" + QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex()) + ".bin");
}
QList<QVariantMap> restoreSnapshot(const Options &o) {
    const QFileInfo folder(o.folder.toLocalFile());
    if (!folder.isDir() || folder.isSymLink()) return {};
    const auto drive = containingDrive(folder.absoluteFilePath());
    QFile file(snapshotPath(o));
    if (file.size() > 16 * 1024 * 1024 || !file.open(QIODevice::ReadOnly)) return {};
    QDataStream stream(&file); stream.setVersion(QDataStream::Qt_6_8);
    quint32 version; QString binding; QList<QVariantMap> rows;
    stream >> version >> binding >> rows;
    if (stream.status() != QDataStream::Ok || version != 1 || binding != (drive ? drive->identifier() : QString())) return {};
    for (const auto &row : rows)
        if (QFileInfo(row.value("filePath").toString()).absolutePath() != folder.absoluteFilePath()) return {};
    return rows;
}
void saveSnapshot(const Options &o, const Listing &listing) {
    const auto path = snapshotPath(o); const auto directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory) || QFileInfo(directory).isSymLink()) return;
    QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_8);
    stream << quint32(1) << (listing.drive ? listing.drive->identifier() : QString()) << listing.rows;
    if (bytes.size() > 16 * 1024 * 1024) return;
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.size() == bytes.size() && existing.readAll() == bytes) return;
    existing.close(); QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        && file.write(bytes) == bytes.size()) file.commit();
    // Directory snapshots are rebuildable. Keep the 128 most recently written.
    static std::mutex pruneMutex;
    std::lock_guard lock(pruneMutex);
    const auto entries = QDir(directory).entryInfoList({"*.bin"}, QDir::Files | QDir::NoSymLinks, QDir::Time);
    qint64 total = 0;
    for (int i = 0; i < entries.size(); ++i) {
        total += entries[i].size();
        if (i >= 128 || total > 128ll * 1024 * 1024) QFile::remove(entries[i].absoluteFilePath());
    }
}
Listing list(const Options &o, const QJsonArray &cached, const QString &cachedStamp,
             const std::shared_ptr<std::atomic_bool> &cancel) {
    Listing result;
    if (!o.folder.isLocalFile() || o.folder.toLocalFile().isEmpty()) return result;
    const auto folder = QDir::cleanPath(o.folder.toLocalFile());
    result.drive = containingDrive(folder);
    QHash<QString, QVariantMap> paths;
    const auto infos = QDir(folder).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::NoSort);
    std::atomic<qsizetype> next = 0;
    auto inspect = [&] {
        QList<QVariantMap> rows;
        while (!cancel->load()) {
            const auto i = next.fetch_add(1); if (i >= infos.size()) break;
            const auto info = infos[i]; const auto path = info.absoluteFilePath();
            rows.append({{"fileName", info.fileName()}, {"filePath", path}, {"fileUrl", QUrl::fromLocalFile(path)},
                {"fileSuffix", info.suffix()}, {"fileIsDir", info.isDir()}, {"fileSize", info.size()},
                {"fileModified", info.lastModified()}, {"filePreviewUrl", QUrl::fromLocalFile(path)}, {"fileResident", true}});
        }
        return rows;
    };
    // Amortize thread creation for small folders; fan out larger metadata walks.
    const auto count = std::min<unsigned>(std::max(1u, std::thread::hardware_concurrency()), std::max<qsizetype>(1, infos.size() / 256));
    std::vector<std::future<QList<QVariantMap>>> scans;
    for (unsigned i = 1; i < count; ++i) scans.push_back(std::async(std::launch::async, inspect));
    auto entries = inspect();
    for (auto &scan : scans) entries.append(scan.get());
    for (const auto &row : entries) {
        paths.insert(row.value("filePath").toString(), row);
    }
    if (result.drive) {
        const auto root = result.drive->rootPath();
        const auto relativeFolder = result.drive->relativePath(folder);
        const QFileInfo catalog(QDir(root).filePath(".society-sync/catalog.json"));
        result.stamp = root + '\n' + result.drive->identifier() + '\n' + QString::number(catalog.size())
            + '\n' + QString::number(catalog.lastModified().toMSecsSinceEpoch())
            + '\n' + QString::number(catalog.metadataChangeTime().toMSecsSinceEpoch());
        StorageMap map(*result.drive);
        const bool authority = map.isLocalAuthority();
        // The host filesystem is authoritative. Parsing every remote/catalogue
        // row cannot improve its local directory listing and adds O(namespace).
        result.catalog = authority ? QJsonArray{} : result.stamp == cachedStamp ? cached : map.objects();
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
        if (match) {
            auto entry = row;
            entry.insert("fileThumbnailUrl", StorageDirectoryModel::thumbnailUrl(row.value("filePreviewUrl").toUrl().toLocalFile()));
            result.rows.append(entry);
        }
    }
    std::sort(result.rows.begin(), result.rows.end(), [&o](const auto &a, const auto &b) {
        if (o.dirsFirst && a.value("fileIsDir") != b.value("fileIsDir")) return a.value("fileIsDir").toBool();
        int comparison;
        if (o.sort == StorageDirectoryModel::Time && a.value("fileModified") != b.value("fileModified")) comparison = a.value("fileModified").toDateTime() > b.value("fileModified").toDateTime() ? -1 : 1;
        else if (o.sort == StorageDirectoryModel::Size && a.value("fileSize") != b.value("fileSize")) comparison = a.value("fileSize").toLongLong() > b.value("fileSize").toLongLong() ? -1 : 1;
        else comparison = a.value("fileName").toString().compare(b.value("fileName").toString(), o.sortCaseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
        if (!comparison) comparison = a.value("filePath").toString().compare(b.value("filePath").toString(), Qt::CaseSensitive);
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
    m_poll.setInterval(10000); m_requestPoll.setInterval(500);
    m_debounce.setSingleShot(true); m_debounce.setInterval(100);
    connect(&m_watches, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_watches, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_debounce, &QTimer::timeout, this, &StorageDirectoryModel::refresh);
    connect(&m_poll, &QTimer::timeout, this, &StorageDirectoryModel::refresh);
    connect(&m_requestPoll, &QTimer::timeout, this, &StorageDirectoryModel::checkRequest);
    connect(this, &StorageDirectoryModel::optionsChanged, this, [this] { ++m_optionsRevision; m_snapshotAttempted = true; refresh(); });
}
StorageDirectoryModel::~StorageDirectoryModel() { if (m_cancel) m_cancel->store(true); }
int StorageDirectoryModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_rows.size(); }
QHash<int, QByteArray> StorageDirectoryModel::roleNames() const {
    QHash<int, QByteArray> roles; int role = Qt::UserRole + 1;
    for (const auto *name : {"fileName", "filePath", "fileUrl", "fileSuffix", "fileIsDir", "fileSize", "fileModified", "filePreviewUrl", "fileResident", "fileThumbnailUrl"}) roles.insert(role++, name);
    return roles;
}
QVariant StorageDirectoryModel::data(const QModelIndex &index, int role) const { return get(index.row(), QString::fromLatin1(roleNames().value(role))); }
QVariant StorageDirectoryModel::get(int index, const QString &role) const { return index >= 0 && index < m_rows.size() ? m_rows[index].value(role) : QVariant(); }
bool StorageDirectoryModel::isFolder(int index) const { return get(index, "fileIsDir").toBool(); }
void StorageDirectoryModel::setFolder(const QUrl &folder) {
    if (m_folder == folder) return;
    if (m_cancel) m_cancel->store(true);
    m_debounce.stop();
    const auto watches = m_watches.files() + m_watches.directories();
    if (!watches.isEmpty()) m_watches.removePaths(watches);
    ++m_revision; ++m_requestRevision; m_running = m_pending = m_checkingRequest = false;
    m_folder = folder; m_drive.reset(); m_catalog = {}; m_catalogStamp.clear();
    m_snapshotAttempted = false;
    m_request.clear(); m_requestDrive.reset(); m_requestPoll.stop();
    if (m_downloading) { m_downloading = false; emit downloadingChanged(); }
    if (!m_rows.isEmpty()) { beginResetModel(); m_rows.clear(); endResetModel(); emit countChanged(); }
    m_status = folder.isLocalFile() && !folder.toLocalFile().isEmpty() ? Loading : Null;
    emit folderChanged(); emit statusChanged();
    if (m_status == Null) { m_poll.stop(); return; }
    m_poll.start(); QTimer::singleShot(0, this, &StorageDirectoryModel::refresh);
}
void StorageDirectoryModel::activate(int index) {
    if (m_status != Ready) return; // Never authorize an open from provisional cached residency.
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
        const auto key = StorageMap::logicalPath(r.drive->relativePath(path));
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
            const auto key = StorageMap::logicalPath(drive.relativePath(path));
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
    if (!m_snapshotAttempted) {
        m_snapshotAttempted = true;
        background(this, [options] { return restoreSnapshot(options); },
            [this, revision, optionsRevision](QList<QVariantMap> rows) {
                if (revision != m_revision) return;
                m_running = false;
                if (optionsRevision == m_optionsRevision && !rows.isEmpty()) { applyRows(rows); emit snapshotRestored(); }
                refresh();
            });
        return;
    }
    const auto cached = m_catalog; const auto stamp = m_catalogStamp;
    background(this, [options, cached, stamp, cancel] { return list(options, cached, stamp, cancel); },
        [this, revision, optionsRevision, options](Listing result) {
            if (revision != m_revision) return;
            if (optionsRevision != m_optionsRevision) {
                m_running = false;
                QTimer::singleShot(0, this, &StorageDirectoryModel::refresh);
                return;
            }
            m_running = false; m_drive = std::move(result.drive); m_catalog = std::move(result.catalog); m_catalogStamp = std::move(result.stamp);
            QStringList wanted{m_folder.toLocalFile()};
            if (m_drive) {
                const auto sync = QDir(m_drive->rootPath()).filePath(".society-sync");
                wanted.append({m_drive->rootPath(), sync, sync + "/catalog.json", sync + "/primary.json", sync + "/previews"});
            }
            // Bound OS watch handles; the fallback poll covers very large directories.
            for (const auto &row : result.rows) {
                if (wanted.size() >= 64) break;
                if (row.value("fileResident").toBool()) wanted.append(row.value("filePath").toString());
            }
            const auto watched = m_watches.files() + m_watches.directories();
            const QSet<QString> old(watched.cbegin(), watched.cend()), next(wanted.cbegin(), wanted.cend());
            const auto remove = (old - next).values(), add = (next - old).values();
            if (!remove.isEmpty()) m_watches.removePaths(remove);
            // The directory watch must be active before Ready. Individual file
            // watches are optional latency hints, installed in frame-sized batches.
            auto pendingWatches = add;
            if (pendingWatches.removeAll(m_folder.toLocalFile())) m_watches.addPath(m_folder.toLocalFile());
            QTimer::singleShot(0, this, [this, pendingWatches, revision] { addWatches(pendingWatches, revision); });
            const bool changed = result.rows != m_rows;
            applyRows(result.rows);
            if (m_status != Ready) { m_status = Ready; emit statusChanged(); }
            // Cache persistence must not delay first paint or live readiness on
            // a busy/slow disk. The bounded snapshot is independent of this model.
            if (changed) background(this, [options, rows = result.rows, drive = m_drive] {
                Listing snapshot; snapshot.rows = rows; snapshot.drive = drive;
                saveSnapshot(options, snapshot); return true;
            }, [](bool) {});
            if (m_pending) QTimer::singleShot(0, this, &StorageDirectoryModel::refresh);
        });
}
void StorageDirectoryModel::addWatches(QStringList paths, quint64 revision) {
    if (revision != m_revision || paths.isEmpty()) return;
    QStringList batch;
    for (int i = 0; i < 8 && !paths.isEmpty(); ++i) {
        const auto path = paths.takeFirst();
        if (QFileInfo::exists(path)) batch.append(path);
    }
    if (!batch.isEmpty()) m_watches.addPaths(batch);
    if (!paths.isEmpty()) QTimer::singleShot(16, this, [this, paths, revision] { addWatches(paths, revision); });
}
QUrl StorageDirectoryModel::thumbnailUrl(const QString &path) {
    static const QStringList extensions{"png", "jpg", "jpeg", "webp", "bmp", "gif", "tif", "tiff", "avif", "heic", "heif", "svg"};
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isAbsolute() || !extensions.contains(info.suffix().toLower())
        || !info.isFile() || info.isSymLink() || info.canonicalFilePath() != QDir::cleanPath(path)) return {};
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(std::filesystem::u8path(path.toStdString()), error);
    if (error) return {};
    const auto encoded = path.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return QUrl(QStringLiteral("image://society-preview/%1/%2-%3-%4").arg(QString::fromLatin1(encoded))
        .arg(info.size()).arg(qint64(modified.time_since_epoch().count())).arg(info.metadataChangeTime().toMSecsSinceEpoch()));
}
QString StorageDirectoryModel::cacheDirectory() {
    const auto configured = qEnvironmentVariable("IISOCIETY_FILE_CACHE_DIRECTORY");
    if (!configured.isEmpty() && QFileInfo(configured).isAbsolute()) return QDir::cleanPath(configured);
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("society");
}
}
