#pragma once

#include "../SocietyDrive.h"
#include <QLockFile>
#include <QSet>
#include <QStringList>
#include <memory>
#include <vector>

namespace iiSocietyContainer::detail {

// A private, per-drive journal. Related Deleted assets share a lease; unrelated
// assets do not hold one another up during a long copy or recursive removal.
class DeletedOrigins final {
public:
    struct Lookup {
        QString original;
        QString recordedItem;
        QString error;
    };
    explicit DeletedOrigins(const SocietyDrive &drive);
    bool lockPaths(const QStringList &deleted, QString *error, int timeoutMs = 2000);
    void unlock();
    bool reserved(const QString &deleted) const;
    bool write(const QString &deleted, const QString &original, bool committed, QString *error);
    Lookup find(const QString &deleted) const;
    void forget(const QString &deleted) const;
private:
    QString recordPath(const QString &deleted) const;
    QString lockKey(const QString &deleted) const;
    bool owns(const QString &deleted) const;
    SocietyDrive m_drive;
    QString m_directory;
    std::vector<std::unique_ptr<QLockFile>> m_locks;
    QSet<QString> m_keys;
};

}
