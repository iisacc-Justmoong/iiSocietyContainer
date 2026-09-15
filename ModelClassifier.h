#pragma once

#include "ModelType.h"
#include <QStringList>
#include <QJsonObject>

namespace iiSocietyContainer {
struct IISOCIETYCONTAINER_EXPORT ModelClassification {
    ModelType type = ModelType::Other;
    bool recognized = false;
    QString evidence;
};

/// Bounded, read-only inspection of metadata and validated safetensors descriptors.
/// Checks shapes/offsets against file size; never deserializes pickle or loads tensors.
class IISOCIETYCONTAINER_EXPORT ModelClassifier {
public:
    static ModelClassification classify(const QString &path, const QString &fileName = {});
    /// Reads bounded catalog metadata and tensor dtype names; never tensor contents.
    static QJsonObject metadata(const QString &path);
    static bool isPackage(const QString &directory);
    static QStringList companionFiles(const QString &path);
};
}
