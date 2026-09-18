#include "FileActions.h"
#include "FileOperations.h"
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QMimeData>
#include <QtConcurrent/QtConcurrentRun>

namespace iiSocietyContainer {
FileActions::FileActions(QObject *parent) : QObject(parent) {
    m_worker.setMaxThreadCount(1);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &FileActions::clipboardChanged);
    connect(&m_download, &StorageDirectoryModel::activated, this, [this] { execute(); });
    connect(&m_download, &StorageDirectoryModel::downloadFailed, this, [this](const QString &error) { finish({}, error); });
}
FileActions::~FileActions() { m_worker.waitForDone(); }
void FileActions::setPath(const QString &path) { if (m_path != path) { m_path = path; emit pathChanged(); } }
bool FileActions::editable() const {
    const auto drive = FileOperations::containingDrive(m_path);
    return drive && FileOperations(*drive).editable(m_path);
}
bool FileActions::inDeleted() const {
    const auto drive = FileOperations::containingDrive(m_path);
    return drive && m_path.startsWith(drive->sectionPath(StoreSection::Deleted) + '/');
}
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
void FileActions::share() { begin(Share, m_path); }
void FileActions::paste(const QString &folder) {
    if (m_busy || !canPaste()) return;
    m_pasteSources.clear();
    for (const auto &url : QGuiApplication::clipboard()->mimeData()->urls()) m_pasteSources.append(url.toLocalFile());
    begin(Paste, m_pasteSources.first(), folder);
}
void FileActions::begin(Operation operation, const QString &source, const QString &argument) {
    if (m_busy) return;
    m_operation = operation; m_source = source; m_argument = argument;
    m_busy = true; m_error.clear(); m_status = tr("Preparing the selected item…"); emit changed();
    if (operation == Paste || operation == Trash || operation == Remove) { execute(); return; }
    // Actions that need contents materialize just their own descendants. Listing or
    // opening the menu never starts a transfer.
    m_download.openPath(source, true);
}
void FileActions::execute() {
    if (m_operation == Clipboard || m_operation == Share) {
        QString error;
        if (m_operation == Clipboard) copyFiles({m_source});
        else shareFiles({m_source}, &error);
        finish(m_source, error); return;
    }
    const auto source = m_source, argument = m_argument; const auto operation = m_operation;
    const auto sources = operation == Paste ? m_pasteSources : QStringList{source};
    auto *watcher = new QFutureWatcher<FileOperations::Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        const auto result = watcher->result(); watcher->deleteLater(); finish(result.path, result.error);
    });
    m_status = m_operation == Trash ? tr("Moving to Deleted…") : m_operation == Remove ? tr("Deleting permanently…")
        : m_operation == Duplicate || m_operation == Paste ? tr("Copying…") : tr("Updating the selected item…"); emit changed();
    watcher->setFuture(QtConcurrent::run(&m_worker, [source, argument, operation, sources] {
        const auto drive = FileOperations::containingDrive(operation == Paste ? argument : source);
        if (!drive) return FileOperations::Result{{}, tr("The Society container is unavailable.")};
        const auto action = operation == Duplicate ? FileOperations::Action::Duplicate
            : operation == Rename ? FileOperations::Action::Rename : operation == Trash ? FileOperations::Action::Trash
            : operation == Remove ? FileOperations::Action::Remove : FileOperations::Action::Copy;
        FileOperations::Result result;
        for (const auto &path : sources) {
            result = FileOperations(*drive).perform(action, path, argument);
            if (!result) break;
        }
        return result;
    }));
}
void FileActions::finish(const QString &path, const QString &error) {
    m_busy = false; m_error = error;
    m_status = !error.isEmpty() ? error : m_operation == Clipboard ? tr("File copied. Use Paste in the destination folder.")
        : m_operation == Trash ? tr("Moved to Deleted. Copy and paste it elsewhere to recover it.")
        : m_operation == Share ? tr("Choose a sharing destination.") : tr("Done.");
    emit changed(); if (error.isEmpty()) emit completed(path); else emit failed(error);
}
#ifndef Q_OS_MACOS
bool FileActions::sharingAvailable() { return false; }
bool FileActions::shareFiles(const QStringList &, QString *error) {
    if (error) *error = tr("Native sharing is unavailable on this platform."); return false;
}
#endif
}
