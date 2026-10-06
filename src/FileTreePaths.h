#pragma once
#include <QString>
#include <filesystem>

namespace iiSocietyContainer::detail {
inline std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}
}
