#pragma once

#include "SocietyDrive.h"
#include <QJsonObject>

namespace iiSocietyContainer {

struct IISOCIETYCONTAINER_EXPORT StoredModel {
    QString id; // Path relative to Models; never a Finder/File Provider replica path.
    QString name;
    QString format; // safetensors or diffusers
    QString fingerprint; // File inventory metadata, not a full weight hash.
    QJsonObject reference(const QString &containerId) const;
};

/// Shared local storage contract for Society and its iisacc application clients.
class IISOCIETYCONTAINER_EXPORT SharedStorage
{
public:
    static QString settingsPath();
    static bool setDefaultContainer(const QString &path, QString *error = nullptr);
    static std::optional<SharedStorage> open(const QString &path = {}, QString *error = nullptr);
    /// Storage-owner bootstrap only. Consumer operations still require isReady().
    static std::optional<SharedStorage> open(const QString &path, QString *error, bool allowIncompleteReplica);

    const SocietyDrive &drive() const;
    QList<StoredModel> models(QString *error = nullptr) const;
    QString resolveModel(const QJsonObject &reference, QString *error = nullptr) const;
    // Native absolute path for QFile/std::filesystem. Empty relativePath returns
    // the section directory. Only the final component may be missing; no writes.
    // Rejects traversal, redirected paths and a replaced/unavailable drive.
    QString filePath(StoreSection section, const QString &relativePath = {}, QString *error = nullptr) const;
    QString ensureDirectory(StoreSection section, const QString &relativePath, QString *error = nullptr) const;

private:
    explicit SharedStorage(SocietyDrive drive);
    SocietyDrive m_drive;
};
}
