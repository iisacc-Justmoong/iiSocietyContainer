#include "FileActions.h"
#include "FileOperations.h"
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMimeData>
#include <QPointer>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <exception>
#include <utility>

namespace iiSocietyContainer {
namespace {
QThreadPool *operationPool() {
    static QPointer<QThreadPool> pool;
    if (!pool) {
        pool = new QThreadPool(QCoreApplication::instance());
        pool->setMaxThreadCount(4);
        pool->setExpiryTimeout(30000);
    }
    return pool;
}
QThreadPool *inspectionPool() {
    static QPointer<QThreadPool> pool;
    if (!pool) {
        pool = new QThreadPool(QCoreApplication::instance());
        pool->setMaxThreadCount(2);
        pool->setExpiryTimeout(30000);
    }
    return pool;
}
struct PathCapabilities { bool editable = false; bool inDeleted = false; };
}

// One request owns its own materialization state. It is attached to the
// application rather than a view so destroying a menu cannot cancel a file
// operation or wait for the worker on the GUI thread.
class FileActions::Request final : public QObject {
public:
    Request(FileActions *owner, quint64 id, Operation operation, QString source,
            QString argument, QStringList sources)
        : QObject(QCoreApplication::instance()), m_owner(owner), m_id(id), m_operation(operation),
          m_source(std::move(source)), m_argument(std::move(argument)), m_sources(std::move(sources)) {}
    void start() {
        if (m_operation == Paste || m_operation == Rename || m_operation == Trash || m_operation == Remove
            || m_operation == Restore || m_operation == RestoreTo) { execute(); return; }
        auto *download = new StorageDirectoryModel(this);
        connect(download, &StorageDirectoryModel::activated, this, [this] { execute(); });
        connect(download, &StorageDirectoryModel::downloadFailed, this, [this](const QString &error) { finish({}, error); });
        download->openPath(m_source, true);
    }
private:
    void execute() {
        if (m_started || m_done) return;
        m_started = true;
        if (m_operation == Clipboard || m_operation == Share) {
            QString error;
            if (m_operation == Clipboard) FileActions::copyFiles({m_source});
            else FileActions::shareFiles({m_source}, &error);
            finish(m_source, error); return;
        }
        const auto source = m_source, argument = m_argument;
        const auto sources = m_operation == Paste ? m_sources : QStringList{m_source};
        const auto operation = m_operation;
        auto *watcher = new QFutureWatcher<FileOperations::Result>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
            const auto result = watcher->result();
            finish(result.path, result.error);
        });
        watcher->setFuture(QtConcurrent::run(operationPool(), [source, argument, operation, sources] {
            try {
            const auto drive = FileOperations::containingDrive(operation == Paste ? argument : source);
            if (!drive) return FileOperations::Result{{}, FileActions::tr("The Society container is unavailable.")};
            const auto action = operation == Duplicate ? FileOperations::Action::Duplicate
                : operation == Rename ? FileOperations::Action::Rename : operation == Trash ? FileOperations::Action::Trash
                : operation == Remove ? FileOperations::Action::Remove : operation == Restore ? FileOperations::Action::Restore
                : operation == RestoreTo ? FileOperations::Action::RestoreTo : FileOperations::Action::Copy;
            FileOperations::Result result;
            for (const auto &path : sources) {
                result = FileOperations(*drive).perform(action, path, argument);
                if (!result) break;
            }
            return result;
            } catch (const std::exception &error) {
                return FileOperations::Result{{}, QString::fromUtf8(error.what())};
            }
        }));
    }
    void finish(const QString &path, const QString &error) {
        if (m_done) return;
        m_done = true;
        if (m_owner) m_owner->finish(m_id, m_operation, m_source, path, error);
        deleteLater();
    }
    QPointer<FileActions> m_owner;
    quint64 m_id;
    Operation m_operation;
    QString m_source, m_argument;
    QStringList m_sources;
    bool m_done = false, m_started = false;
};

FileActions::FileActions(QObject *parent) : QObject(parent) {
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &FileActions::clipboardChanged);
}
FileActions::~FileActions() = default;
void FileActions::setPath(const QString &path) {
    if (m_path == path) return;
    m_path = path;
    const auto epoch = ++m_pathEpoch;
    m_editable = false;
    m_inDeleted = false;
    emit pathChanged();
    emit eligibilityChanged();
    if (path.isEmpty()) return;
    auto *watcher = new QFutureWatcher<PathCapabilities>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, epoch] {
        const auto capabilities = watcher->result();
        watcher->deleteLater();
        if (epoch != m_pathEpoch) return;
        m_editable = capabilities.editable;
        m_inDeleted = capabilities.inDeleted;
        emit eligibilityChanged();
    });
    watcher->setFuture(QtConcurrent::run(inspectionPool(), [path] {
        try {
            const auto drive = FileOperations::containingDrive(path);
            if (!drive) return PathCapabilities{};
            return PathCapabilities{FileOperations(*drive).editable(path),
                path.startsWith(drive->sectionPath(StoreSection::Deleted) + '/')};
        } catch (const std::exception &) { return PathCapabilities{}; }
    }));
}
bool FileActions::busyForPath(const QString &path) const {
    if (path.isEmpty()) return false;
    auto clean = QDir::cleanPath(path);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    clean = clean.normalized(QString::NormalizationForm_D).toCaseFolded();
#endif
    for (const auto &pending : m_operations) {
        auto other = QDir::cleanPath(pending);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        other = other.normalized(QString::NormalizationForm_D).toCaseFolded();
#endif
        if (clean == other || clean.startsWith(other + '/') || other.startsWith(clean + '/')) return true;
    }
    return false;
}
bool FileActions::editable() const { return m_editable; }
bool FileActions::inDeleted() const { return m_inDeleted; }
bool FileActions::canPaste() const {
    const auto *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || mime->urls().isEmpty() || mime->urls().size() > 128) return false;
    for (const auto &url : mime->urls()) if (!url.isLocalFile()) return false;
    return true;
}
void FileActions::copyFiles(const QStringList &paths) {
    QList<QUrl> urls; for (const auto &path : paths) urls.append(QUrl::fromLocalFile(path));
    auto *mime = new QMimeData; mime->setUrls(urls); QGuiApplication::clipboard()->setMimeData(mime);
}
void FileActions::copyPath() { if (!m_path.isEmpty()) { QGuiApplication::clipboard()->setText(m_path); m_status = tr("Path copied."); m_error.clear(); emit changed(); } }
void FileActions::copy() { begin(Clipboard, m_path); }
void FileActions::duplicate() { begin(Duplicate, m_path); }
void FileActions::rename(const QString &name) { begin(Rename, m_path, name); }
void FileActions::trash() { begin(Trash, m_path); }
void FileActions::remove() { begin(Remove, m_path); }
void FileActions::removePath(const QString &path) { begin(Remove, path); }
void FileActions::restore() { begin(Restore, m_path); }
void FileActions::restoreTo(const QUrl &folder) {
    const auto local = folder.isLocalFile() ? folder.toLocalFile() : QString();
    begin(RestoreTo, m_path, local.isEmpty() ? QString() : QDir::cleanPath(local));
}
void FileActions::share() { begin(Share, m_path); }
void FileActions::paste(const QString &folder) {
    if (!canPaste()) return;
    QStringList sources;
    for (const auto &url : QGuiApplication::clipboard()->mimeData()->urls()) sources.append(url.toLocalFile());
    begin(Paste, sources.first(), folder, sources);
}
void FileActions::begin(Operation operation, const QString &source, const QString &argument, const QStringList &sources) {
    if (source.isEmpty() || busyForPath(source) || m_operations.size() >= 128) {
        m_error = source.isEmpty() ? tr("Select a file first.") : busyForPath(source)
            ? tr("An operation on this item is already in progress.") : tr("Too many file operations are pending.");
        m_status = m_error; emit changed(); emit operationFinished(source, {}, m_error); emit failed(m_error); return;
    }
    const auto id = ++m_nextOperation;
    m_operations.insert(id, source);
    m_error.clear();
    m_status = operation == Trash ? tr("Moving to Deleted…") : operation == Remove ? tr("Removing from Deleted…")
        : operation == Restore || operation == RestoreTo ? tr("Putting back…")
        : operation == Duplicate || operation == Paste ? tr("Copying…") : tr("Preparing the selected item…");
    emit changed();
    (new Request(this, id, operation, source, argument, sources))->start();
}
void FileActions::finish(quint64 id, Operation operation, const QString &source, const QString &path, const QString &error) {
    if (!m_operations.remove(id)) return;
    m_error = error;
    m_status = !error.isEmpty() ? error : busy() ? tr("%1 file operation(s) in progress.").arg(activeCount())
        : operation == Clipboard ? tr("File copied. Use Paste in the destination folder.")
        : operation == Trash ? tr("Moved to Deleted. Put it back to restore its original location.")
        : operation == Restore ? tr("Returned to its original location.") : operation == RestoreTo ? tr("Returned to the selected folder.")
        : operation == Share ? tr("Choose a sharing destination.") : tr("Done.");
    emit changed();
    emit operationFinished(source, path, error);
    if (error.isEmpty()) emit completed(path);
    else {
        if (!path.isEmpty()) emit partiallyCompleted(path, error);
        emit failed(error);
    }
}
#ifndef Q_OS_MACOS
bool FileActions::sharingAvailable() { return false; }
bool FileActions::shareFiles(const QStringList &, QString *error) {
    if (error) *error = tr("Native sharing is unavailable on this platform."); return false;
}
#endif
}
