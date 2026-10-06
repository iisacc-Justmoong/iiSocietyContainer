#pragma once

#include "iiSocietyContainer.h"

#include <optional>

namespace iiSocietyContainer {

/// A persistent drive with nine independent, ordinary directory sections.
/// Native system adapters and application browsers share this same layout.
class IISOCIETYCONTAINER_EXPORT SocietyDrive
{
public:
    /// Creates the ordinary Society folder beneath an existing parent directory.
    /// Selecting an existing drive reuses its identity. No disk image is created.
    [[nodiscard]] static std::optional<SocietyDrive> createAt(
        const QString& parentDirectory, QString* error = nullptr);
    /// Initializes an existing directory, or opens its existing valid drive.
    /// Conflicting entries and unknown manifests are preserved and rejected.
    /// Finder CloudStorage replicas and descendants of another drive are rejected
    /// before initialization; open() enforces the same source-location boundary.
    [[nodiscard]] static std::optional<SocietyDrive> create(
        const QString& directoryPath, QString* error = nullptr);
    /// Opening a ready drive creates any missing fixed Files directories.
    /// Existing contents and the manifest are preserved; conflicting entries fail.
    [[nodiscard]] static std::optional<SocietyDrive> open(
        const QString& directoryPath, QString* error = nullptr);

    /// After authenticated host selection, adopt its logical drive identity.
    /// The caller owns migration of contents; the section paths stay local.
    /// expectedIdentifier prevents overwriting a concurrently replaced drive.
    [[nodiscard]] static std::optional<SocietyDrive> adoptReplicaIdentity(
        const QString& directoryPath, const QString& expectedIdentifier,
        const QString& hostIdentifier, QString* error = nullptr);
    /// Publish a fully downloaded mirror to local native adapters.
    [[nodiscard]] static bool completeReplica(const QString& directoryPath,
        const QString& expectedIdentifier, QString* error = nullptr);

    [[nodiscard]] QString identifier() const;
    /// Public drive label is Society, including when opening a legacy manifest.
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QString rootPath() const;
    [[nodiscard]] bool isValid() const;
    /// False while the first host snapshot is being applied, or after replacement.
    [[nodiscard]] bool isReady() const;
    [[nodiscard]] QList<StoreSection> sections() const;
    [[nodiscard]] QString sectionPath(StoreSection section) const;
    [[nodiscard]] std::optional<StoreSection> sectionForPath(const QString& path) const;
    /// Map the logical section namespace to native paths, including the Files volume.
    [[nodiscard]] QString resolvePath(const QString& relative) const;
    [[nodiscard]] QString relativePath(const QString& absolute) const;

    /// Live logical tree of the nine sections, including a separate Files volume.
    /// Empty paths select the logical root; paths use display names ("Files/a").
    /// Unready or replaced drives fail. Only resident native entries are returned.
    [[nodiscard]] FileTree::EntryResult entry(const QString& relativePath = {}) const;
    [[nodiscard]] FileTree::ChildrenResult entries(const QString& relativeDirectory = {},
                                                 bool includeHidden = false) const;
    [[nodiscard]] FileTree::EntryResult tree(const QString& relativePath = {},
                                            FileTree::Options options = {}) const;

private:
    SocietyDrive(QString root, QString identifier);
    QString m_rootPath;
    QString m_identifier;
};

} // namespace iiSocietyContainer
