#pragma once

#include "ModelType.h"

namespace iiSocietyContainer::detail {
// Validate existing categories without materializing unused taxonomy folders.
bool validateModelLayout(const QString &root, QString *error);
// A writer prepares only the category it is about to use.
bool ensureModelCategory(const QString &root, ModelType type, QString *error);
}
