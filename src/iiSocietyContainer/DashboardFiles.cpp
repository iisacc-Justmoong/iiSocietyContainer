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

namespace iiSocietyContainer {
namespace {
constexpr qsizetype dashboardListLimit = 20;
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
    for (const auto section : {iiSocietyContainer::StoreSection::Files,
                              iiSocietyContainer::StoreSection::GenerationHistory}) {
        const bool history = section == iiSocietyContainer::StoreSection::GenerationHistory;
        result.watches.append(drive->sectionPath(section));
        QDirIterator iterator(drive->sectionPath(section), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Readable,
                              history ? QDirIterator::NoIteratorFlags : QDirIterator::Subdirectories);
        while (!cancel->load() && iterator.hasNext()) {
            iterator.next();
            const auto file = iterator.fileInfo();
            if (file.isSymLink()) continue;
            // Recheck SDK boundaries in case an entry changed during the walk.
            const auto actualSection = drive->sectionForPath(file.canonicalFilePath());
            if (!actualSection || *actualSection != section) continue;
            if (file.isDir()) {
                if (!history) result.watches.append(file.canonicalFilePath());
                continue;
            }
            if (!file.isFile() || (history && !isImage(file))) continue;
            result.watches.append(file.canonicalFilePath());
            const auto suffix = file.suffix().toLower();
            const auto kind = isImage(file) ? QStringLiteral("Image")
                : suffix == "iisc" ? QStringLiteral("Canvas") : QStringLiteral("Document");
            const auto icon = isImage(file) ? QStringLiteral("fileTypesimage")
                : suffix == "iisc" ? QStringLiteral("application") : QStringLiteral("fileTypestext");
            const auto folder = QDir(path).relativeFilePath(file.absolutePath());
            auto preview = isImage(file) ? QUrl::fromLocalFile(file.canonicalFilePath()) : QUrl();
            if (!preview.isEmpty()) preview.setQuery(QStringLiteral("v=%1-%2")
                .arg(file.lastModified().toMSecsSinceEpoch()).arg(file.size()));
            result.files.append(QVariantMap{
                {"path", file.canonicalFilePath()}, {"folderPath", file.absolutePath()},
                {"name", file.fileName()}, {"description", iiSocietyContainer::storeSectionName(section) + " · " + kind},
                {"modified", file.lastModified().toUTC()}, {"history", history}, {"iconName", icon},
                {"previewSource", preview},
                {"metadata1", (suffix.isEmpty() ? kind : suffix.toUpper()) + " · " + QLocale().formattedDataSize(file.size())},
                {"metadata2", QStringLiteral("Society / ") + folder}
            });
        }
        if (cancel->load()) return {};
    }
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

QVariantList DashboardFiles::filtered(bool history) const
{
    QVariantList result;
    for (const auto &entry : m_files) {
        auto file = entry.toMap();
        if (file.value("history").toBool() != history) continue;
        if (!m_query.trimmed().isEmpty() && !file.value("name").toString().contains(m_query.trimmed(), Qt::CaseInsensitive)
            && !file.value("metadata2").toString().contains(m_query.trimmed(), Qt::CaseInsensitive)) continue;
        file.insert("dateText", file.value("modified").toDateTime().toLocalTime().date().toString(Qt::ISODate));
        result.append(file);
        if (result.size() == dashboardListLimit) break;
    }
    return result;
}

QVariantList DashboardFiles::recentFiles() const { return filtered(false); }
QVariantList DashboardFiles::generationHistory() const { return filtered(true); }

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
        const QSet<QString> next(result.watches.cbegin(), result.watches.cend());
        const auto removed = (previous - next).values(), added = (next - previous).values();
        if (!removed.isEmpty()) m_watches.removePaths(removed);
        if (!added.isEmpty()) m_watches.addPaths(added);
        const bool changed = m_files != result.files || m_error != result.error;
        m_files = result.files; m_error = result.error; m_loading = false;
        if (changed) emit filesChanged();
        emit loadingChanged();
        if (m_refreshPending) { m_refreshPending = false; m_debounce.start(); }
    });
    watcher->setFuture(QtConcurrent::run(scan, m_path, m_cancel));
}

} // namespace iiSocietyContainer
