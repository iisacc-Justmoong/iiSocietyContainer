#pragma once

#include <QString>

namespace iiSocietyContainer::detail {
// A single filesystem operation. It never replaces a raced-in target and
// never falls back to copying the contents of a large model.
bool renameWithoutCopy(const QString &source, const QString &destination);
}
