#pragma once

#include "iiSocietyContainer.h"

#include <optional>

namespace iiSocietyContainer {

/// A persistent drive with eight independent, ordinary directory sections.
/// Native system adapters and application browsers share this same layout.
class IISOCIETYCONTAINER_EXPORT SocietyDrive
{
public:
    /// Initializes an existing directory, or opens its existing valid drive.
    /// Conflicting entries and unknown manifests are preserved and rejected.
    /// Finder CloudStorage replicas and descendants of another drive are rejected
    /// before initialization; open() enforces the same source-location boundary.
    [[nodiscard]] static std::optional<SocietyDrive> create(
        const QString& directoryPath, QString* error = nullptr);
    [[nodiscard]] static std::optional<SocietyDrive> open(
        const QString& directoryPath, QString* error = nullptr);

    [[nodiscard]] QString identifier() const;
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QString rootPath() const;
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QList<StoreSection> sections() const;
    [[nodiscard]] QString sectionPath(StoreSection section) const;
    [[nodiscard]] std::optional<StoreSection> sectionForPath(const QString& path) const;

private:
    SocietyDrive(QString root, QString identifier);
    QString m_rootPath;
    QString m_identifier;
};

} // namespace iiSocietyContainer
