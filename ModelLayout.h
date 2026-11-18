#pragma once

#include "ModelType.h"
#include <QStringList>
#include <stop_token>

namespace iiSocietyContainer::detail {
// Validate existing categories without creating or following redirected paths.
bool validateModelLayout(const QString &root, QString *error);
// Create missing categories without replacing existing entries. On failure,
// remove only empty directories created by this call.
bool ensureModelLayout(const QString &root, QString *error, std::stop_token stop = {},
                       QStringList *createdDirectories = nullptr);
bool ensureModelCategory(const QString &root, ModelType type, QString *error);
}
