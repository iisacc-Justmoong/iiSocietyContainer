#include "DashboardFiles.h"
#include <iiFileProviderPresentation.h>
#include <SocietyDrive.h>
#include <SharedStorage.h>
#include <QJsonArray>
#include <QRegularExpression>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLocale>
#include <QSet>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <future>
#include <utility>
#include <StorageDirectoryModel.h>

namespace iiSocietyContainer {
namespace {
constexpr qsizetype dashboardListLimit = 20;
constexpr qsizetype recentPublishedLimit = 4;
struct Snapshot { QVariantList files; QStringList watches; QString error; };
struct Search { QSet<QString> paths; QString error; };
Search searchIndex(const QString &root, const QString &text, std::stop_token stop)
{
    Search result;
    const auto storage = SharedStorage::open(root, &result.error); if (!storage) return result;
    // Standalone consumers can search before a daemon has ever indexed the
    // drive. Bootstrap on this I/O worker; subsequent requests use indexes only.
    if (!std::filesystem::exists(FileRepository::packagePath(std::filesystem::path(root.toStdU16String()), storage->drive().identifier().toStdString()))
        && !storage->reconcileFiles(&result.error, stop)) return result;
    QJsonObject request; QStringList words, keywords, collections; QJsonArray conditions;
    const auto tokens = text.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    for (const auto &token : tokens) {
        const auto colon = token.indexOf(':'); const auto kind = token.left(colon).toLower(), value = token.mid(colon + 1);
        if (colon <= 0) { words.append(token); continue; }
        if (kind == "class") request.insert("classKey", value);
        else if (kind == "in") request.insert("directory", value);
        else if (kind == "keyword") keywords.append(value);
        else if (kind == "collection") collections.append(value);
        else if (kind == "color") request.insert("color", value.toLower());
        else if (kind == "rating") {
            bool ok = false; const int rating = value.toInt(&ok);
            if (!ok || rating < 0 || rating > 5) { result.error = "Use rating:0 through rating:5."; return result; }
            request.insert("minimumRating", rating);
        } else { words.append(token); }
    }
    const bool semantic = request.contains("classKey") || request.contains("directory");
    if (request.contains("directory") && (request.contains("classKey") || !words.isEmpty() || !keywords.isEmpty()
        || !collections.isEmpty() || request.contains("color") || request.contains("minimumRating"))) {
        result.error = "Use in: with a saved semantic directory by itself."; return result;
    }
    if (semantic) {
        if (!words.isEmpty()) { result.error = "Use class: or in: with keyword:, color: and rating: filters."; return result; }
        for (const auto &keyword : keywords) conditions.append(QJsonObject{{"property","society:keyword"},{"equals",QJsonObject{{"kind","text"},{"value",keyword}}}});
        for (const auto &collection : collections) conditions.append(QJsonObject{{"property","society:collection"},{"equals",QJsonObject{{"kind","text"},{"value",collection}}}});
        if (request.contains("color")) { conditions.append(QJsonObject{{"property","society:color"},{"equals",QJsonObject{{"kind","text"},{"value",request.take("color")}}}}); }
        if (request.contains("minimumRating")) { conditions.append(QJsonObject{{"property","society:rating"},{"equals",QJsonObject{{"kind","integer"},{"value",QString::number(request.take("minimumRating").toInt())}}}}); }
        if (!conditions.isEmpty()) request.insert("conditions", conditions);
    } else {
        if (!words.isEmpty()) request.insert("text", words.join(' '));
        if (!keywords.isEmpty()) request.insert("keywords", QJsonArray::fromStringList(keywords));
        if (!collections.isEmpty()) request.insert("collections", QJsonArray::fromStringList(collections));
    }
    for (const auto section : {StoreSection::Files, StoreSection::Published, StoreSection::GenerationHistory}) {
        if (!semantic) request.insert("section", storeSectionKey(section));
        request.remove("cursor"); request.insert("limit", 1024);
        for (int pages = 0; pages < 256; ++pages) {
            if (stop.stop_requested()) return {};
            const auto page = storage->searchFiles(request, &result.error); if (!result.error.isEmpty()) return result;
            for (const auto &value : page.value("files").toArray()) result.paths.insert(value.toObject().value("path").toString().normalized(QString::NormalizationForm_C));
            const auto cursor = page.value("next").toObject(); if (cursor.isEmpty()) break;
            request.insert("cursor", cursor);
        }
        if (semantic) break;
    }
    return result;
}

bool isImage(const QFileInfo &file)
{
    static const QStringList extensions{"png", "jpg", "jpeg", "webp", "bmp", "gif", "tif", "tiff", "avif", "heic", "heif"};
    return extensions.contains(file.suffix().toLower());
}

Snapshot scan(const QString &path, const std::shared_ptr<std::atomic_bool> &cancel)
{
    Snapshot result;
    if (QFileInfo(path).isDir() && !QFileInfo(path).isSymLink()) result.watches.append(path);
    const auto drive = iiSocietyContainer::SocietyDrive::open(path, &result.error);
    if (!drive) return result;
    result.watches.append(QDir(path).filePath(".society-drive.json"));
    // Display published local contents independently of the transport's mirror
    // inspection. An unfinished initial replica is still not a readable store.
    if (!drive->isReady()) return result;
    auto scanSection = [&](iiSocietyContainer::StoreSection section) {
        Snapshot result;
        const bool history = section == iiSocietyContainer::StoreSection::GenerationHistory;
        const auto sectionRoot = drive->sectionPath(section);
        if (QFileInfo(sectionRoot).canonicalFilePath() != sectionRoot) return result;
        result.watches.append(sectionRoot);
        QDirIterator iterator(sectionRoot, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Readable,
                              history ? QDirIterator::NoIteratorFlags : QDirIterator::Subdirectories);
        while (!cancel->load() && iterator.hasNext()) {
            iterator.next();
            const auto file = iterator.fileInfo();
            const auto components = QDir(sectionRoot).relativeFilePath(file.filePath()).split('/');
            if (std::any_of(components.begin(), components.end(), [](const auto &part) {
                return part.startsWith('.') || iiFileProvider::native::Presentation::isControlFile(
                    std::filesystem::path(part.toStdU16String()));
            })) continue;
            if (file.isSymLink() || file.fileName().startsWith('.')
                || iiFileProvider::native::Presentation::isControlFile(std::filesystem::path(file.fileName().toStdU16String()))) continue;
            // Recheck SDK boundaries in case an entry changed during the walk.
            const auto canonical = file.canonicalFilePath();
            if (!canonical.startsWith(sectionRoot + '/')) continue;
            if (file.isDir()) {
                if (!history) result.watches.append(canonical);
                continue;
            }
            // History owns generated image originals. Editor canvas wrappers
            // remain documents and must not duplicate an image in this gallery.
            if (!file.isFile() || (history && !isImage(file))) continue;
            result.watches.append(canonical);
            const auto suffix = file.suffix().toLower();
            const auto kind = isImage(file) ? QStringLiteral("Image")
                : suffix == "iisc" ? QStringLiteral("Canvas") : QStringLiteral("Document");
            const auto icon = isImage(file) ? QStringLiteral("fileTypesimage")
                : suffix == "iisc" ? QStringLiteral("application") : QStringLiteral("fileTypestext");
            const auto folder = QDir(path).relativeFilePath(file.absolutePath());
            auto preview = isImage(file) ? QUrl::fromLocalFile(canonical) : QUrl();
            if (!preview.isEmpty()) preview.setQuery(QStringLiteral("v=%1-%2")
                .arg(file.lastModified().toMSecsSinceEpoch()).arg(file.size()));
            const auto thumbnail = StorageDirectoryModel::thumbnailUrl(canonical);
            result.files.append(QVariantMap{
                {"path", canonical}, {"folderPath", file.absolutePath()},
                {"name", file.fileName()}, {"description", iiSocietyContainer::storeSectionName(section) + " · " + kind},
                {"modified", file.lastModified().toUTC()}, {"history", history},
                {"section", iiSocietyContainer::storeSectionKey(section)}, {"iconName", icon},
                {"previewSource", preview},
                {"thumbnailSource", thumbnail},
                {"metadata1", (suffix.isEmpty() ? kind : suffix.toUpper()) + " · " + QLocale().formattedDataSize(file.size())},
                {"metadata2", QStringLiteral("Society / ") + folder}
            });
        }
        return result;
    };
    // Independent sections run concurrently; none occupy the GUI event loop.
    auto files = std::async(std::launch::async, scanSection, StoreSection::Files);
    auto published = std::async(std::launch::async, scanSection, StoreSection::Published);
    auto history = scanSection(StoreSection::GenerationHistory);
    for (auto part : {files.get(), published.get(), history}) {
        result.files.append(part.files); result.watches.append(part.watches);
    }
    if (cancel->load()) return {};
    if (!drive->isReady()) {
        result.files.clear();
        result.error = DashboardFiles::tr("The Society drive changed while its files were being read.");
        return result;
    }
    std::sort(result.files.begin(), result.files.end(), [](const QVariant &left, const QVariant &right) {
        const auto a = left.toMap(), b = right.toMap();
        const auto at = a.value("modified").toDateTime(), bt = b.value("modified").toDateTime();
        return at == bt ? a.value("path").toString() < b.value("path").toString() : at > bt;
    });
    return result;
}

}

DashboardFiles::DashboardFiles(QObject *parent) : QObject(parent)
{
    m_searchDebounce.setSingleShot(true); m_searchDebounce.setInterval(120);
    connect(&m_searchDebounce, &QTimer::timeout, this, &DashboardFiles::search);
    m_debounce.setSingleShot(true); m_debounce.setInterval(150);
    connect(&m_watches, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_watches, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_debounce, &QTimer::timeout, this, &DashboardFiles::refresh);
    m_fallback.setInterval(10000);
    connect(&m_fallback, &QTimer::timeout, this, &DashboardFiles::refresh);
}
DashboardFiles::~DashboardFiles() { if (m_cancel) m_cancel->store(true); m_searchStop.request_stop(); if (m_searchWatcher) m_searchWatcher->waitForFinished(); }

void DashboardFiles::setContainerPath(const QString &path)
{
    if (m_path == path) return;
    if (m_cancel) m_cancel->store(true);
    ++m_revision; m_refreshPending = false; m_debounce.stop();
    m_loading = false;
    const auto watches = m_watches.files() + m_watches.directories();
    if (!watches.isEmpty()) m_watches.removePaths(watches);
    m_path = path;
    m_searchStop.request_stop();
    ++m_searchRevision; m_completedQuery.clear(); m_matches.clear();
    if (path.isEmpty()) m_fallback.stop(); else m_fallback.start();
    m_files.clear(); m_error.clear();
    emit containerPathChanged(); emit filesChanged();
    refresh();
}

void DashboardFiles::setQuery(const QString &query)
{
    if (m_query == query) return;
    m_query = query;
    m_searchStop.request_stop();
    ++m_searchRevision; m_searchDebounce.start();
    emit queryChanged(); emit filesChanged();
}

QVariantList DashboardFiles::filtered(StoreSection section, qsizetype limit) const
{
    QVariantList result;
    const auto key = storeSectionKey(section);
    for (const auto &entry : m_files) {
        auto file = entry.toMap();
        if (file.value("section").toString() != key) continue;
        if (!m_query.trimmed().isEmpty()) {
            if (m_completedQuery == m_query) {
                const auto root = storeSectionName(section) + '/';
                const auto native = file.value("path").toString();
                const auto relative = QDir(m_path).relativeFilePath(native);
                const auto logical = key + '/' + relative.mid(root.size());
                if (!m_matches.contains(logical.normalized(QString::NormalizationForm_C))) continue;
            } else if (!file.value("name").toString().contains(m_query.trimmed(), Qt::CaseInsensitive)
                && !file.value("metadata2").toString().contains(m_query.trimmed(), Qt::CaseInsensitive)) continue;
        }
        file.insert("dateText", file.value("modified").toDateTime().toLocalTime().date().toString(Qt::ISODate));
        result.append(file);
        if (result.size() == limit) break;
    }
    return result;
}

QVariantList DashboardFiles::recentFiles() const { return filtered(StoreSection::Files, dashboardListLimit); }
QVariantList DashboardFiles::recentPublished() const { return filtered(StoreSection::Published, recentPublishedLimit); }
QVariantList DashboardFiles::generationHistory() const { return filtered(StoreSection::GenerationHistory, dashboardListLimit); }
void DashboardFiles::search()
{
    if (m_searching) { m_searchPending = true; return; }
    if (m_query.trimmed().isEmpty() || m_path.isEmpty()) { m_completedQuery.clear(); m_matches.clear(); emit filesChanged(); return; }
    m_searching = true; emit searchChanged(); const auto revision = m_searchRevision;
    m_searchStop = std::stop_source{}; const auto stop = m_searchStop.get_token();
    const auto path = m_path, query = m_query;
    auto *watcher = new QFutureWatcher<Search>(this);
    m_searchWatcher = watcher;
    connect(watcher, &QFutureWatcher<Search>::finished, this, [this, watcher, revision, query] {
        const auto result = watcher->result(); m_searchWatcher = nullptr; watcher->deleteLater(); m_searching = false; emit searchChanged();
        if (revision == m_searchRevision) { m_matches = result.paths; m_completedQuery = query; m_error = result.error; emit filesChanged(); }
        const bool again = std::exchange(m_searchPending, false) || revision != m_searchRevision;
        if (again) m_searchDebounce.start();
    });
    watcher->setFuture(QtConcurrent::run([path, query, stop] { try { return searchIndex(path, query, stop); } catch (const std::exception &e) { return Search{{},QString::fromUtf8(e.what())}; } }));
}

void DashboardFiles::refresh()
{
    if (m_loading) { m_refreshPending = true; return; }
    m_debounce.stop();
    const auto revision = ++m_revision;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    // Empty or relative paths must never enumerate the process working directory.
    if (m_path.isEmpty() || !QFileInfo(m_path).isAbsolute()) {
        m_files.clear();
        m_error.clear();
        if (!m_path.isEmpty()) m_error = tr("Choose an absolute Society drive path.");
        m_loading = false; emit loadingChanged(); emit filesChanged();
        return;
    }
    m_loading = true; emit loadingChanged();
    auto *watcher = new QFutureWatcher<Snapshot>(this);
    connect(watcher, &QFutureWatcher<Snapshot>::finished, this, [this, watcher, revision] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (revision != m_revision) return;
        const auto watches = m_watches.files() + m_watches.directories();
        const QSet<QString> previous(watches.cbegin(), watches.cend());
        // Bound native watch resources. The periodic background scan covers
        // deeper paths while the visible, newest files get immediate edit signals.
        QStringList roots{m_path, QDir(m_path).filePath(".society-drive.json")};
        for (const auto section : {StoreSection::Files, StoreSection::Published, StoreSection::GenerationHistory})
            roots.append(QDir(m_path).filePath(storeSectionName(section)));
        auto wanted = roots + result.watches.mid(0, 64);
        for (const auto section : {StoreSection::Files, StoreSection::Published, StoreSection::GenerationHistory}) {
            int count = 0;
            for (const auto &entry : result.files) {
                const auto file = entry.toMap();
                if (file.value("section") != storeSectionKey(section)) continue;
                wanted.append(file.value("path").toString());
                if (++count == 20) break;
            }
        }
        const QSet<QString> next(wanted.cbegin(), wanted.cend());
        const auto removed = (previous - next).values(); auto added = (next - previous).values();
        if (!removed.isEmpty()) m_watches.removePaths(removed);
        QStringList immediate;
        for (const auto &root : roots) if (added.removeAll(root) && QFileInfo::exists(root)) immediate.append(root);
        if (!immediate.isEmpty()) m_watches.addPaths(immediate);
        if (!added.isEmpty()) QTimer::singleShot(0, this, [this, added, revision] { addWatches(added, revision); });
        const bool changed = m_files != result.files || m_error != result.error;
        m_files = result.files; m_error = result.error; m_loading = false;
        if (changed) emit filesChanged();
        emit loadingChanged();
        if (!m_query.trimmed().isEmpty()) m_searchDebounce.start();
        if (m_refreshPending) { m_refreshPending = false; m_debounce.start(); }
    });
    watcher->setFuture(QtConcurrent::run(scan, m_path, m_cancel));
}
void DashboardFiles::addWatches(QStringList paths, quint64 revision) {
    if (revision != m_revision || paths.isEmpty()) return;
    const auto batch = paths.mid(0, 8); paths.remove(0, batch.size());
    m_watches.addPaths(batch);
    if (!paths.isEmpty()) QTimer::singleShot(16, this, [this, paths, revision] { addWatches(paths, revision); });
}

} // namespace iiSocietyContainer
