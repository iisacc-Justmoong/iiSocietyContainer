#pragma once
#include "SharedStorage.h"

namespace iiSocietyContainer {
// Shared Society storage adapter. Mobile applications without a shared store
// use MobileStorage for their app-private container until migration is possible.
class IISOCIETYCONTAINER_EXPORT ApplicationStorage final {
public:
    // Resolves without creating storage. Windows/Linux use a per-user suite data
    // root; Apple uses its entitled App Group. Desktop deployments may supply
    // IISACC_SHARED_STORAGE_ROOT, an absolute canonical shared directory.
    static QString sharedRoot(QString *error = nullptr);
    // Compatibility name; this is not an Apple-only operation.
    static QString sharedGroupRoot(QString *error = nullptr);
    static QString selectedRoot(const QString &sharedGroup, QString *error = nullptr);
    static QJsonObject registration(const QString &sharedGroup, QString *error = nullptr);
    // Existing selections take precedence. Only this explicit bootstrap creates
    // a missing suite root; app-private Documents/cache are never substituted.
    static std::optional<SharedStorage> openForApplication(const QString &application, QString *error = nullptr);
    // Portable policy for a canonical shared root supplied by its owner. Apple
    // production callers above obtain the root from the native App Group API.
    static std::optional<SharedStorage> openInGroup(const QString &sharedGroup, const QString &application, QString *error = nullptr);
    static std::optional<SharedStorage> adoptInGroup(const QString &sharedGroup, QString *error = nullptr);
};
}
