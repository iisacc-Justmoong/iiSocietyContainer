#pragma once

#include "iiSocietyContainer.h"

#include <optional>

namespace iiSocietyContainer {

/// A persistent package or explicitly declared logical disk with nine sections.
/// Native system adapters and application browsers share this same layout.
class IISOCIETYCONTAINER_EXPORT SocietyDrive
{
public:
    enum class StorageKind { Package, LogicalDisk };
    [[nodiscard]] static std::optional<SocietyDrive> createLogical(
        const QString &directoryPath, const QString &ownerApplication, QString *error = nullptr);
    [[nodiscard]] StorageKind storageKind() const;
    [[nodiscard]] QString ownerApplication() const;
    /// Creates Society.societycontainer beneath an existing parent directory.
    /// Selecting an existing drive reuses its identity. No disk image is created.
    [[nodiscard]] static std::optional<SocietyDrive> createAt(
        const QString& parentDirectory, QString* error = nullptr);
    /// Initializes an existing .societycontainer package, or opens its valid drive.
    /// Conflicting entries and unknown manifests are preserved and rejected.
    /// Finder CloudStorage replicas and descendants of another drive are rejected
    /// before initialization; open() enforces the same source-location boundary.
    [[nodiscard]] static std::optional<SocietyDrive> create(
        const QString& directoryPath, QString* error = nullptr);
    /// Opens a canonical package or an explicitly owned logical disk with a valid
    /// manifest. Bare directories, redirects and legacy images are rejected.
    /// Empty current layouts create no extra state; supported layout upgrades
    /// preserve the existing contents and UUID.
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
    /// Stable device-local storage identity, retained when a host UUID is adopted.
    [[nodiscard]] QString storageIdentifier() const;
    /// Public drive label is Society, including when opening a legacy manifest.
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QString rootPath() const;
    [[nodiscard]] bool isValid() const;
    /// False while the first host snapshot is being applied, or after replacement.
    [[nodiscard]] bool isReady() const;
    [[nodiscard]] QList<StoreSection> sections() const;
    [[nodiscard]] QString sectionPath(StoreSection section) const;
    [[nodiscard]] std::optional<StoreSection> sectionForPath(const QString& path) const;
    /// Map the logical section namespace to paths within the current package.
    /// Missing components are allowed; replaced identity and existing redirects fail.
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
