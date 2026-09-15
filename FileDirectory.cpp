#include "FileDirectory.h"

#include <QDir>
#include <QFileInfo>
#include <array>

namespace iiSocietyContainer {
namespace {
struct Definition { FileDirectoryKind kind; const char *key; const char *name; };
constexpr std::array definitions{
    Definition{FileDirectoryKind::Documents, "documents", "Documents"},
    Definition{FileDirectoryKind::Audios, "audios", "Audios"},
    Definition{FileDirectoryKind::Objects3D, "objects3d", "3D objects"}
};
}
QList<FileDirectoryKind> allFileDirectoryKinds()
{
    QList<FileDirectoryKind> result;
    for (const auto &entry : definitions) result.append(entry.kind);
    return result;
}
QString fileDirectoryKey(FileDirectoryKind kind)
{
    for (const auto &entry : definitions) if (entry.kind == kind) return QString::fromLatin1(entry.key);
    return {};
}
QString fileDirectoryName(FileDirectoryKind kind)
{
    for (const auto &entry : definitions) if (entry.kind == kind) return QString::fromLatin1(entry.name);
    return {};
}
bool isFixedFilesDirectory(const QString &relativePath)
{
    for (const auto &entry : definitions)
        if (relativePath.compare(QLatin1String(entry.name), Qt::CaseInsensitive) == 0) return true;
    return false;
}
FileDirectory::FileDirectory(SocietyDrive drive, FileDirectoryKind kind)
    : m_drive(std::move(drive)), m_kind(kind) {}
FileDirectoryKind FileDirectory::kind() const { return m_kind; }
QString FileDirectory::key() const { return fileDirectoryKey(m_kind); }
QString FileDirectory::name() const { return fileDirectoryName(m_kind); }
QString FileDirectory::path() const
{
    if (!m_drive.isReady() || name().isEmpty()) return {};
    const auto path = QDir(m_drive.sectionPath(StoreSection::Files)).filePath(name());
    const QFileInfo info(path);
    return info.isDir() && !info.isSymLink() && !info.isJunction() && info.canonicalFilePath() == path ? path : QString();
}
bool FileDirectory::isValid() const { return !path().isEmpty(); }
}
