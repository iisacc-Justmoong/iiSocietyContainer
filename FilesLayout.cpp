#include "FilesLayout.h"
#include <QDir>
#include <QFileInfo>

namespace iiSocietyContainer::detail {
bool removeLegacyFilesDirectories(const QString &root, QString *error)
{
    for (const auto *name : {"Documents", "Audios", "3D objects"}) {
        const auto path = QDir(root).filePath(QLatin1String(name));
        const QFileInfo info(path);
        if (!info.isDir() || info.isSymLink() || info.isJunction()
            || info.canonicalFilePath() != path) continue;
        const auto entries = QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
        if (!QDir(path).entryList(entries).isEmpty()) continue;
        // rmdir is non-recursive and also preserves content added concurrently.
        if (!QDir().rmdir(path) && QFileInfo::exists(path) && QDir(path).entryList(entries).isEmpty()) {
            if (error) *error = QStringLiteral("Could not remove an empty legacy Files directory: %1").arg(path);
            return false;
        }
    }
    return true;
}
}
