#pragma once

#include "FileDirectory.h"
#include <QFileInfo>

namespace iiSocietyContainer {

/// Public filesystem projection. Its root is Files, never the container root.
/// Native adapters use relative paths; no section IDs or source paths are public.
class IISOCIETYCONTAINER_EXPORT FilesView final {
public:
    static std::optional<FilesView> open(const QString &containerPath, QString *error = nullptr);
    const SocietyDrive &drive() const;
    QString rootPath() const;
    bool isValid() const;
    // An empty path selects the root. Only a missing final component is allowed.
    QString resolve(const QString &relativePath, bool allowMissing = false, QString *error = nullptr) const;
    QList<QFileInfo> entries(const QString &relativeDirectory = {}, QString *error = nullptr) const;
    QList<FileDirectory> directories() const;
    std::optional<FileDirectory> directory(FileDirectoryKind kind) const;
    /// The public root and four fixed directory names cannot be deleted, moved,
    /// renamed, or replaced. Native adapters must check both move endpoints.
    static bool isProtectedPath(const QString &relativePath);

private:
    explicit FilesView(SocietyDrive drive);
    SocietyDrive m_drive;
};
}
