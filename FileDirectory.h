#pragma once

#include "SocietyDrive.h"

namespace iiSocietyContainer {

// Keep the retired Photos value for ABI compatibility; directory(Photos) returns
// no value. Photos now uses SocietyDrive::sectionPath(StoreSection::Photos).
enum class FileDirectoryKind { Documents, Photos, Audios, Objects3D };

IISOCIETYCONTAINER_EXPORT QList<FileDirectoryKind> allFileDirectoryKinds();
IISOCIETYCONTAINER_EXPORT QString fileDirectoryKey(FileDirectoryKind kind);
IISOCIETYCONTAINER_EXPORT QString fileDirectoryName(FileDirectoryKind kind);
/// Reserved immediate children of Files, including case aliases used by native filesystems.
/// Descendants and equally named directories elsewhere are ordinary user entries.
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
    bool isProtected() const { return true; }

private:
    friend class FilesView;
    FileDirectory(SocietyDrive drive, FileDirectoryKind kind);
    SocietyDrive m_drive;
    FileDirectoryKind m_kind;
};
}
