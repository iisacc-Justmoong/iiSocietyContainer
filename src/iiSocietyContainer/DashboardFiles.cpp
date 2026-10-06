#include "DashboardFiles.h"
#include <SocietyDrive.h>
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
#include <StorageDirectoryModel.h>

namespace iiSocietyContainer {
namespace {
constexpr qsizetype dashboardListLimit = 20;
constexpr qsizetype recentPublishedLimit = 4;
struct Snapshot { QVariantList files; QStringList watches; QString error; };

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
            if (file.isSymLink() || file.fileName().startsWith('.')) continue;
            // Recheck SDK boundaries in case an entry changed during the walk.
            const auto canonical = file.canonicalFilePath();
            if (!canonical.startsWith(sectionRoot + '/')) continue;
            if (file.isDir()) {
                if (!history) result.watches.append(canonical);
                continue;
            }
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
            const auto thumbnail = isImage(file) ? StorageDirectoryModel::thumbnailUrl(canonical) : QUrl();
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
    m_debounce.setSingleShot(true); m_debounce.setInterval(150);
    connect(&m_watches, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_watches, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_debounce, &QTimer::timeout, this, &DashboardFiles::refresh);
    m_fallback.setInterval(10000);
    connect(&m_fallback, &QTimer::timeout, this, &DashboardFiles::refresh);
}
DashboardFiles::~DashboardFiles() { if (m_cancel) m_cancel->store(true); }

void DashboardFiles::setContainerPath(const QString &path)
{
    if (m_path == path) return;
    if (m_cancel) m_cancel->store(true);
    ++m_revision; m_refreshPending = false; m_debounce.stop();
    m_loading = false;
    const auto watches = m_watches.files() + m_watches.directories();
    if (!watches.isEmpty()) m_watches.removePaths(watches);
    m_path = path;
    if (path.isEmpty()) m_fallback.stop(); else m_fallback.start();
    m_files.clear(); m_error.clear();
    emit containerPathChanged(); emit filesChanged();
    refresh();
}

void DashboardFiles::setQuery(const QString &query)
{
    if (m_query == query) return;
    m_query = query;
    emit queryChanged(); emit filesChanged();
}

QVariantList DashboardFiles::filtered(StoreSection section, qsizetype limit) const
{
    QVariantList result;
    const auto key = storeSectionKey(section);
    for (const auto &entry : m_files) {
        auto file = entry.toMap();
        if (file.value("section").toString() != key) continue;
        if (!m_query.trimmed().isEmpty() && !file.value("name").toString().contains(m_query.trimmed(), Qt::CaseInsensitive)
            && !file.value("metadata2").toString().contains(m_query.trimmed(), Qt::CaseInsensitive)) continue;
        file.insert("dateText", file.value("modified").toDateTime().toLocalTime().date().toString(Qt::ISODate));
        result.append(file);
        if (result.size() == limit) break;
    }
    return result;
}

QVariantList DashboardFiles::recentFiles() const { return filtered(StoreSection::Files, dashboardListLimit); }
QVariantList DashboardFiles::recentPublished() const { return filtered(StoreSection::Published, recentPublishedLimit); }
QVariantList DashboardFiles::generationHistory() const { return filtered(StoreSection::GenerationHistory, dashboardListLimit); }

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
