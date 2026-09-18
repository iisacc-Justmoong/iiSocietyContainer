#pragma once
#include "SocietyDrive.h"
#include <QJsonArray>
#include <QJsonObject>

namespace iiSocietyContainer {
// A device-local projection of the host namespace. Metadata is not a placeholder
// file: absent payloads never become zero-byte files or implicit deletions.
class IISOCIETYCONTAINER_EXPORT StorageMap final {
public:
    explicit StorageMap(const SocietyDrive &drive);
    QJsonArray objects(QString *error = nullptr) const;
    // A declared local host browses its actual files while its index catches up.
    // Replicas continue to display absent, remote-only objects from the catalog.
    bool isLocalAuthority() const;
    QJsonObject object(const QString &key) const;
    QStringList files(const QString &key) const;
    bool available(const QStringList &keys) const;
    bool isResident(const QJsonObject &object) const;
    QString localPath(const QString &key) const;
    QString previewPath(const QJsonObject &object) const;
    // Durable, same-device request; a Society SDK session consumes it. A request
    // pins versions and survives app switches/restarts. No credential is stored.
    QString request(const QStringList &keys, QString *error = nullptr) const;
    QJsonObject requestState(const QString &id) const;
    bool cancel(const QString &id) const;
    QJsonArray pendingRequests() const;
    bool finishRequest(const QString &id, const QString &error = {}) const;
    bool publish(const QJsonArray &objects, QString *error = nullptr) const;
    static QString physicalPath(const QString &key);
    static QString logicalPath(const QString &relativePath);
private:
    bool prepare(QString *error = nullptr) const;
    bool write(const QString &relative, const QJsonObject &value, QString *error = nullptr) const;
    QJsonObject read(const QString &relative, qint64 limit) const;
    SocietyDrive m_drive;
};
}
