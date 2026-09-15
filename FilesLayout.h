#pragma once
#include <QStringList>

namespace iiSocietyContainer::detail {
bool validateFilesLayout(const QString &root, QString *error);
bool createFilesLayout(const QString &root, QString *error, QStringList *created = nullptr);
}
