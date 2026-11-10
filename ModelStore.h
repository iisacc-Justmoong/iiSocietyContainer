#pragma once

#include "ModelClassifier.h"
#include "SocietyDrive.h"
#include <QPair>
#include <atomic>
#include <stop_token>

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
    static QList<ModelEntry> scanDirectory(const QString &modelsDirectory, QString *error,
        const std::atomic_bool *cancelled, std::stop_token stop);
    QString rootPath() const;
    /// Prepare only this requested category for model publication. Other
    /// logical categories remain absent until explicitly used by a writer.
    QString categoryPath(ModelType type, QString *error = nullptr) const;
    /// Validate existing taxonomy paths; never create unused type folders.
    bool ensureLayout(QString *error = nullptr) const;
    bool ensureLayout(QString *error, std::stop_token stop) const;
    QList<ModelEntry> entries(QString *error = nullptr, const std::atomic_bool *cancelled = nullptr) const;
    QList<ModelEntry> entries(QString *error, const std::atomic_bool *cancelled, std::stop_token stop) const;
    /// Index an existing model asset without capturing its full payload.
    /// The returned catalog key identifies this working copy; it is not a
    /// retained immutable content revision.
    QString indexAsset(const QString &path, QString *error = nullptr) const;
    /// Only existing entries inside Models may move. An explicit Other selection
    /// is retained by automatic organization, including for an existing Other item.
    QString place(const QString &path, std::optional<ModelType> type = {}, QString *error = nullptr) const;
    /// Rename one model in its current folder, together with its companions.
    /// Existing destinations are never replaced and the former path remains an alias.
    QString rename(const QString &path, const QString &name, QString *error = nullptr,
                   QList<QPair<QString, QString>> *movedAssets = nullptr) const;
    /// Recover detached provider companions only when catalog and native file
    /// identity prove the destination. Ambiguous data is preserved and reported.
    QStringList repairCompanions(QStringList *errors = nullptr, const std::atomic_bool *cancelled = nullptr) const;
    QStringList repairCompanions(QStringList *errors, const std::atomic_bool *cancelled, std::stop_token stop) const;
    /// Preserves named categories and manual Other selections; retries other unknowns.
    ModelOrganization organize(const std::atomic_bool *cancelled = nullptr) const;
    ModelOrganization organize(const std::atomic_bool *cancelled, std::stop_token stop) const;
    /// Resolves a current or previously organized relative model path, within this container only.
    QString resolve(const QString &relativePath, QString *error = nullptr) const;
private:
    explicit ModelStore(SocietyDrive drive);
    QString relocate(const QString &path, std::optional<ModelType> type,
                     const std::optional<QString> &name, QString *error,
                     QList<QPair<QString, QString>> *movedAssets = nullptr) const;
    SocietyDrive m_drive;
};
}
