#include "ModelLayout.h"
#include "ModelType.h"

#include <QDir>
#include <QFileInfo>

namespace iiSocietyContainer::detail {
bool validateModelLayout(const QString &root, QString *error)
{
    for (const auto type : allModelTypes()) {
        const auto path = QDir(root).filePath(modelTypeName(type));
        const QFileInfo info(path);
        if ((info.exists() || info.isSymLink() || info.isJunction())
            && (!info.isDir() || info.isSymLink() || info.isJunction() || info.canonicalFilePath() != path)) {
            if (error) *error = QStringLiteral("A model category conflicts with an existing entry: %1").arg(path);
            return false;
        }
    }
    return true;
}
bool createModelLayout(const QString &root, QString *error, QStringList *created)
{
    if (!validateModelLayout(root, error)) return false;
    for (const auto type : allModelTypes()) {
        const auto path = QDir(root).filePath(modelTypeName(type));
        if (!QFileInfo::exists(path)) {
            if (!QDir().mkdir(path)) {
                if (error) *error = QStringLiteral("Could not create model category: %1").arg(path);
                return false;
            }
            if (created) created->append(path);
        }
    }
    return true;
}
}
