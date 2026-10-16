#pragma once
#include <QString>
#ifdef Q_OS_MACOS
#include <QFile>
#include <QFileInfo>
#endif
#include <filesystem>

namespace iiSocietyContainer::detail {
inline std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#elif defined(Q_OS_MACOS)
    auto native = std::filesystem::path(QFile::encodeName(path).toStdString());
    const QFileInfo info(path);
    // Qt presents decomposed macOS filenames as composed QStrings. Only a
    // direct Qt path may adopt the filesystem's actual Unicode spelling;
    // aliases retain their original path for the native confinement checks.
    if (info.exists() && !info.isSymLink() && !info.isJunction() && info.canonicalFilePath() == path) {
        std::error_code error;
        auto canonical = std::filesystem::canonical(native, error);
        if (!error) return canonical;
    }
    return native;
#else
    return std::filesystem::path(path.toStdString());
#endif
}
}
