#pragma once

#include "iiSocietyContainerExport.h"
#include "src/Store/StoreSection.h"

#include <QtCore/QList>
#include <QtCore/QString>

namespace iiSocietyContainer {

/// An existing directory explicitly designated as a logical SocietyContainer space.
class IISOCIETYCONTAINER_EXPORT SocietyContainer
{
public:
    enum class PathKind {
        Outside, ///< Outside the space, missing, or invalid.
        Root,    ///< The directory designated as this SocietyContainer.
        Entry    ///< An existing file or directory inside this space.
    };

    /// Resolves a native directory path against the current working directory.
    /// Does not create or modify any filesystem entries. Symlinks are resolved.
    explicit SocietyContainer(const QString& directoryPath);

    /// Whether the designated canonical path still resolves to that directory path.
    [[nodiscard]] bool isValid() const;

    /// The fixed canonical absolute root; empty if construction failed.
    [[nodiscard]] QString rootPath() const;

    /// A diagnostic for an invalid container; empty while valid.
    [[nodiscard]] QString errorString() const;

    /// Classifies an existing native path after resolving symlinks.
    /// Relative paths are resolved against rootPath(), not the working directory.
    /// Empty paths, missing entries and invalid containers return Outside.
    [[nodiscard]] PathKind classifyPath(const QString& path) const;

    /// Returns all eight logical sections while the container is valid, else none.
    /// Sections exist independently of files, directories, and their names.
    [[nodiscard]] QList<StoreSection> sections() const;

    /// Whether this valid container provides the specified logical section.
    [[nodiscard]] bool hasSection(StoreSection section) const;

private:
    QString m_rootPath;
    QString m_errorString;
};

/// Returns the original greeting for compatibility with existing consumers.
[[nodiscard]] IISOCIETYCONTAINER_EXPORT QString helloWorld();

} // namespace iiSocietyContainer
