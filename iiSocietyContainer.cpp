#include "iiSocietyContainer.h"
#include "src/FileTreePaths.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

namespace iiSocietyContainer {

SocietyContainer::SocietyContainer(const QString& directoryPath)
{
    if (directoryPath.isEmpty() || directoryPath.contains(QChar::Null)) {
        m_errorString = QStringLiteral("A non-empty directory path without null characters is required.");
        return;
    }

    const QFileInfo directory(directoryPath);
    if (!directory.isNativePath() || !directory.isDir()) {
        m_errorString = QStringLiteral("The path is not an existing native directory: %1")
                            .arg(directoryPath);
        return;
    }

    m_rootPath = directory.canonicalFilePath();
    if (m_rootPath.isEmpty()) {
        m_errorString = QStringLiteral("The directory path could not be resolved: %1")
                            .arg(directoryPath);
    }
}

bool SocietyContainer::isValid() const
{
    if (m_rootPath.isEmpty()) {
        return false;
    }

    const QFileInfo directory(m_rootPath);
    return directory.isDir() && directory.canonicalFilePath() == m_rootPath;
}

QString SocietyContainer::rootPath() const
{
    return m_rootPath;
}

QString SocietyContainer::errorString() const
{
    if (!m_errorString.isEmpty()) {
        return m_errorString;
    }
    if (!isValid()) {
        return QStringLiteral("The SocietyContainer directory is no longer available at: %1")
            .arg(m_rootPath);
    }
    return {};
}

SocietyContainer::PathKind SocietyContainer::classifyPath(const QString& path) const
{
    if (!isValid() || path.isEmpty() || path.contains(QChar::Null)) {
        return PathKind::Outside;
    }

    const QDir root(m_rootPath);
    const QFileInfo entry(root, path);
    if (!entry.isNativePath()) {
        return PathKind::Outside;
    }

    const QString canonicalPath = entry.canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        return PathKind::Outside;
    }

    const QString relativePath = root.relativeFilePath(canonicalPath);
    if (relativePath == QStringLiteral(".")) {
        return PathKind::Root;
    }
    if (QDir::isAbsolutePath(relativePath) || relativePath == QStringLiteral("..")
        || relativePath.startsWith(QStringLiteral("../"))) {
        return PathKind::Outside;
    }
    return PathKind::Entry;
}

QList<StoreSection> SocietyContainer::sections() const
{
    return isValid() ? allStoreSections() : QList<StoreSection>{};
}

FileTree::EntryResult SocietyContainer::entry(const QString& relativePath) const
{
    if (!isValid()) return {std::nullopt, std::make_error_code(std::errc::no_such_file_or_directory)};
    return FileTree(detail::nativePath(m_rootPath)).entry(detail::nativePath(relativePath));
}

FileTree::ChildrenResult SocietyContainer::entries(const QString& relativeDirectory, bool includeHidden) const
{
    if (!isValid()) return {std::nullopt, std::make_error_code(std::errc::no_such_file_or_directory)};
    return FileTree(detail::nativePath(m_rootPath)).children(detail::nativePath(relativeDirectory), includeHidden);
}

FileTree::EntryResult SocietyContainer::tree(const QString& relativePath, FileTree::Options options) const
{
    if (!isValid()) return {std::nullopt, std::make_error_code(std::errc::no_such_file_or_directory)};
    return FileTree(detail::nativePath(m_rootPath)).snapshot(detail::nativePath(relativePath), options);
}

bool SocietyContainer::hasSection(StoreSection section) const
{
    return isValid() && allStoreSections().contains(section);
}

QString helloWorld()
{
    return QStringLiteral("Hello world!");
}

} // namespace iiSocietyContainer
