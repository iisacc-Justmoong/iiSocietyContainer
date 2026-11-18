#include <iiFileProviderDevice.h>
#include "ModelLayout.h"
#include "ModelType.h"
#include <QFileInfo>

namespace iiSocietyContainer::detail {
namespace {
bool directDirectory(const QString &path)
{
    const QFileInfo info(path);
    return info.isDir() && !info.isSymLink() && !info.isJunction()
        && info.canonicalFilePath() == path;
}
}
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
bool ensureModelLayout(const QString &root, QString *error, std::stop_token stop,
                       QStringList *createdDirectories)
{
    if (!directDirectory(root)) {
        if (error) *error = QStringLiteral("The Models directory is invalid or redirected.");
        return false;
    }
    if (!validateModelLayout(root, error)) return false;
    QStringList made;
    const auto rollback = [&made] {
        for (auto it = made.crbegin(); it != made.crend(); ++it)
            iiFileProvider::Directory().rmdir(*it); // Never remove nonempty directories.
    };
    for (const auto type : allModelTypes()) {
        if (stop.stop_requested() || !directDirectory(root)) {
            if (error) *error = stop.stop_requested() ? QStringLiteral("Model organization cancelled.")
                                                       : QStringLiteral("The Models directory changed.");
            rollback(); return false;
        }
        const auto path = iiFileProvider::Directory(root).filePath(modelTypeName(type));
        const QFileInfo info(path);
        if (!info.exists() && !info.isSymLink() && !info.isJunction()) {
            if (iiFileProvider::Directory().mkdir(path)) made.append(path);
            else if (!directDirectory(path)) {
                if (error) *error = QStringLiteral("Could not create model category: %1").arg(path);
                rollback(); return false;
            }
        }
        if (!directDirectory(path)) {
            if (error) *error = QStringLiteral("A model category conflicts with an existing entry: %1").arg(path);
            rollback(); return false;
        }
    }
    if (stop.stop_requested() || !directDirectory(root) || !validateModelLayout(root, error)) {
        if (error && error->isEmpty()) *error = stop.stop_requested() ? QStringLiteral("Model organization cancelled.")
                                                                       : QStringLiteral("The model layout changed.");
        rollback(); return false;
    }
    if (createdDirectories) createdDirectories->append(made);
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
