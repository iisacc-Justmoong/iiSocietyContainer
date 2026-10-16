#include <iiFileProviderDevice.h>
#include "ModelLayout.h"
#include "ModelType.h"
#include <QFileInfo>

namespace iiSocietyContainer::detail {
bool validateModelLayout(const QString &root, QString *error)
{
    for (const auto type : allModelTypes()) {
        const auto path = iiFileProvider::Directory(root).filePath(modelTypeName(type));
        const QFileInfo info(path);
        if ((info.exists() || info.isSymLink() || info.isJunction())
            && (!info.isDir() || info.isSymLink() || info.isJunction() || info.canonicalFilePath() != path)) {
            if (error) *error = QStringLiteral("A model category conflicts with an existing entry: %1").arg(path);
            return false;
        }
    }
    return true;
}
bool ensureModelCategory(const QString &root, ModelType type, QString *error)
{
    const QFileInfo parent(root);
    const auto name = modelTypeName(type);
    if (name.isEmpty() || !parent.isDir() || parent.isSymLink() || parent.isJunction()
        || parent.canonicalFilePath() != root) {
        if (error) *error = QStringLiteral("The requested model category or Models directory is invalid.");
        return false;
    }
    if (!validateModelLayout(root, error)) return false;
    const auto path = iiFileProvider::Directory(root).filePath(name);
    if (!QFileInfo::exists(path)) {
        if (!iiFileProvider::Directory().mkdir(path)) {
            if (error) *error = QStringLiteral("Could not create model category: %1").arg(path);
            return false;
        }
    }
    return validateModelLayout(root, error);
}
}
