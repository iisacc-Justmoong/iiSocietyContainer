#include "FilesView.h"
#include <QDir>

namespace iiSocietyContainer {
namespace {
void fail(QString *error, const QString &message) { if (error) *error = message; }
}

FilesView::FilesView(SocietyDrive drive) : m_drive(std::move(drive)) {}
std::optional<FilesView> FilesView::open(const QString &path, QString *error)
{
    auto drive = SocietyDrive::open(path, error);
    return drive ? std::optional<FilesView>(FilesView(std::move(*drive))) : std::nullopt;
}
const SocietyDrive &FilesView::drive() const { return m_drive; }
QList<FileDirectory> FilesView::directories() const
{
    return {};
}
std::optional<FileDirectory> FilesView::directory(FileDirectoryKind) const
{
    return {};
}
bool FilesView::isProtectedPath(const QString &relativePath)
{
    return relativePath.isEmpty();
}
bool FilesView::isValid() const { return m_drive.isValid(); }
QString FilesView::rootPath() const { return m_drive.sectionPath(StoreSection::Files); }
QString FilesView::resolve(const QString &relative, bool allowMissing, QString *error) const
{
    fail(error, {});
    QString current = rootPath();
    if (current.isEmpty()) {
        fail(error, QStringLiteral("The Society drive is unavailable or its identity changed."));
        return {};
    }
    if (relative.isEmpty()) return current;
    const auto parts = relative.split('/');
    if (QDir::isAbsolutePath(relative) || relative.contains('\\') || relative.contains(':')
        || relative.contains(QChar::Null)) {
        fail(error, QStringLiteral("Use a relative path inside Society Files."));
        return {};
    }
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const auto &part = parts[i];
        if (part.isEmpty() || part == "." || part == "..") {
            fail(error, QStringLiteral("The path leaves the Society Files root."));
            return {};
        }
        current = QDir(current).filePath(part);
        const QFileInfo info(current);
        if (info.isSymLink() || info.isJunction()) {
            fail(error, QStringLiteral("Redirected entries are not exposed by Society Files."));
            return {};
        }
        const bool last = i == parts.size() - 1;
        if (last && allowMissing && !info.exists()) return current;
        if (info.canonicalFilePath() != current || (!last && !info.isDir())
            || (last && !info.isDir() && !info.isFile())) {
            fail(error, QStringLiteral("The Society Files entry is unavailable."));
            return {};
        }
    }
    return current;
}
QList<QFileInfo> FilesView::entries(const QString &relative, QString *error) const
{
    const auto directory = resolve(relative, false, error);
    if (directory.isEmpty() || !QFileInfo(directory).isDir()) {
        fail(error, QStringLiteral("The Society Files folder is unavailable."));
        return {};
    }
    QList<QFileInfo> result;
    for (const auto &entry : QDir(directory).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name)) {
        const auto child = relative.isEmpty() ? entry.fileName() : relative + '/' + entry.fileName();
        if (!resolve(child).isEmpty()) result.append(entry);
    }
    return result;
}
}
