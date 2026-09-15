#include "FilesLayout.h"
#include "FileDirectory.h"
#include <QDir>
#include <QFileInfo>

namespace iiSocietyContainer::detail {
bool validateFilesLayout(const QString &root, QString *error)
{
    for (const auto kind : allFileDirectoryKinds()) {
        const auto path = QDir(root).filePath(fileDirectoryName(kind));
        const QFileInfo info(path);
        if ((info.exists() || info.isSymLink() || info.isJunction())
            && (!info.isDir() || info.isSymLink() || info.isJunction() || info.canonicalFilePath() != path)) {
            if (error) *error = QStringLiteral("A fixed Files directory conflicts with an existing entry: %1").arg(path);
            return false;
        }
    }
    return true;
}
bool createFilesLayout(const QString &root, QString *error, QStringList *created)
{
    if (!validateFilesLayout(root, error)) return false;
    for (const auto kind : allFileDirectoryKinds()) {
        const auto path = QDir(root).filePath(fileDirectoryName(kind));
        if (!QFileInfo::exists(path)) {
            if (QDir().mkdir(path)) {
                if (created) created->append(path);
            } else {
                // Another reader can create the same missing directory concurrently.
                const QFileInfo info(path);
                if (!info.isDir() || info.isSymLink() || info.isJunction() || info.canonicalFilePath() != path) {
                    if (error) *error = QStringLiteral("Could not create fixed Files directory: %1").arg(path);
                    return false;
                }
            }
        }
    }
    return validateFilesLayout(root, error);
}
}
