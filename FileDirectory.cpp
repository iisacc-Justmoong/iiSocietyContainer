#include "FileDirectory.h"

namespace iiSocietyContainer {
// Retain the exported ABI for existing SDK consumers. Files has no built-in directories.
QList<FileDirectoryKind> allFileDirectoryKinds() { return {}; }
QString fileDirectoryKey(FileDirectoryKind) { return {}; }
QString fileDirectoryName(FileDirectoryKind) { return {}; }
bool isFixedFilesDirectory(const QString &) { return false; }
FileDirectory::FileDirectory(SocietyDrive drive, FileDirectoryKind kind)
    : m_drive(std::move(drive)), m_kind(kind) {}
FileDirectoryKind FileDirectory::kind() const { return m_kind; }
QString FileDirectory::key() const { return {}; }
QString FileDirectory::name() const { return {}; }
QString FileDirectory::path() const { return {}; }
bool FileDirectory::isValid() const { return false; }
}
