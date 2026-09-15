#pragma once

#include "ModelClassifier.h"
#include "SocietyDrive.h"
#include <atomic>

namespace iiSocietyContainer {
struct IISOCIETYCONTAINER_EXPORT ModelEntry {
    QString path;
    QString relativePath;
    QString name;
    QString kind; // file, diffusers, adapter, or package
    QString format;
    ModelClassification classification;
};
struct IISOCIETYCONTAINER_EXPORT ModelMove {
    QString previousPath;
    QString path;
    ModelType type = ModelType::Other;
};
struct IISOCIETYCONTAINER_EXPORT ModelOrganization {
    QList<ModelMove> moved;
    QStringList errors;
    bool cancelled = false;
};

/// Owns the Models taxonomy, package inventory, and non-overwriting classification moves.
/// Calls perform filesystem I/O; GUI applications should invoke them on a worker thread.
class IISOCIETYCONTAINER_EXPORT ModelStore {
public:
    static std::optional<ModelStore> open(const QString &containerPath, QString *error = nullptr);
    static QList<ModelEntry> scanDirectory(const QString &modelsDirectory, QString *error = nullptr,
        const std::atomic_bool *cancelled = nullptr);
    QString rootPath() const;
    QString categoryPath(ModelType type, QString *error = nullptr) const;
    bool ensureLayout(QString *error = nullptr) const;
    QList<ModelEntry> entries(QString *error = nullptr, const std::atomic_bool *cancelled = nullptr) const;
    /// Only existing entries inside Models may move. An explicit type supports manual correction.
    QString place(const QString &path, std::optional<ModelType> type = {}, QString *error = nullptr) const;
    /// Preserves existing named categories; retries metadata detection in Other.
    ModelOrganization organize(const std::atomic_bool *cancelled = nullptr) const;
    /// Resolves a current or previously organized relative model path, within this container only.
    QString resolve(const QString &relativePath, QString *error = nullptr) const;
private:
    explicit ModelStore(SocietyDrive drive);
    SocietyDrive m_drive;
};
}
