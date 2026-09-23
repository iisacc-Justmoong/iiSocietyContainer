#pragma once

#include "SocietyDrive.h"

namespace iiSocietyContainer {

// Retired identifiers retained for source and ABI compatibility. No built-in
// directories are provided; use FilesView::entries() for user-created items.
enum class FileDirectoryKind { Documents, Photos, Audios, Objects3D };

IISOCIETYCONTAINER_EXPORT QList<FileDirectoryKind> allFileDirectoryKinds();
IISOCIETYCONTAINER_EXPORT QString fileDirectoryKey(FileDirectoryKind kind);
IISOCIETYCONTAINER_EXPORT QString fileDirectoryName(FileDirectoryKind kind);
/// Compatibility query: no Files child name is reserved.
IISOCIETYCONTAINER_EXPORT bool isFixedFilesDirectory(const QString &relativePath);

/// A directory object bound to one container identity. Paths are resolved anew
/// before use. No content is auto-classified.
class IISOCIETYCONTAINER_EXPORT FileDirectory final {
public:
    FileDirectoryKind kind() const;
    QString key() const;
    QString name() const;
    QString path() const;
    bool isValid() const;
    bool isProtected() const { return false; }

private:
    friend class FilesView;
    FileDirectory(SocietyDrive drive, FileDirectoryKind kind);
    SocietyDrive m_drive;
    FileDirectoryKind m_kind;
};
}
