#pragma once
#include <QString>

namespace iiSocietyContainer::detail {
// One-time upgrade of old containers; never removes contents or redirects.
bool removeLegacyFilesDirectories(const QString &root, QString *error);
}
