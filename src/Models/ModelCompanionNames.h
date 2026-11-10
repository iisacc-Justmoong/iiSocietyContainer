#pragma once

#include <QFileInfo>
#include <QStringList>

namespace iiSocietyContainer::detail {

// Construct names without touching the filesystem; a replica may have only
// catalog entries for the model and its companion files.
inline QStringList modelCompanionNames(const QString &name)
{
    const auto fileName = QFileInfo(name).fileName();
    const auto stem = QFileInfo(fileName).completeBaseName();
    QStringList names;
    for (const auto &prefix : {fileName, stem}) {
        for (const auto *suffix : {".model.json", ".civitai.info", ".preview.png", ".preview.jpg", ".preview.webp"}) {
            const auto candidate = prefix + QLatin1String(suffix);
            if (candidate != fileName && !names.contains(candidate)) names.append(candidate);
        }
    }
    return names;
}

}
