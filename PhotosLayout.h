#pragma once
#include <QString>

namespace iiSocietyContainer::detail {
// Called under the container layout lock while upgrading the legacy manifest.
bool migratePhotosLayout(const QString &root, QString *error, const QString &filesRoot = {});
}
