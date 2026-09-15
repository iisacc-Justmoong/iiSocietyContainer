#pragma once

#include <QStringList>

namespace iiSocietyContainer::detail {
// Internal filesystem layout shared by drive initialization and model management.
bool validateModelLayout(const QString &root, QString *error);
bool createModelLayout(const QString &root, QString *error, QStringList *created = nullptr);
}
