#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace iiFileProvider { class TemporaryFile; }

namespace iiSocietyContainer::detail {

// The container treats a primary file, model attachments and provider-owned
// metadata as one operation. Only primary and associated assets are catalogued;
// provider metadata and previews are internal members of those assets.
class AssetObject final {
public:
    enum class ImportFailure { None, LockTimeout };
    struct Plan {
        QList<QPair<QString, QString>> assets;
        QList<QPair<QString, QString>> members;
        QStringList reservedTargets;
    };
    static std::optional<Plan> plan(const QString &source, const QString &destination,
                                    bool model, bool stabilize, QString *error);
    // Publish an already-copied payload and all accessible local companions.
    // A provider-only source may pass an empty source when siblings are absent.
    static QString publishImport(iiFileProvider::TemporaryFile &payload, const QString &source,
                                 const QString &directory, const QString &name,
                                 const std::shared_ptr<std::atomic_bool> &cancelled, QString *error,
                                 ImportFailure *failure = nullptr,
                                 const std::function<void()> &waiting = {},
                                 const std::function<bool(const QString &)> &catalogReserved = {});
};

// Resolve provider-owned files belonging to a native asset. This only plans
// paths; the caller validates its storage boundary and publishes the group.
bool appendAssetCompanions(const QString &source, const QString &destination,
                           QList<QPair<QString, QString>> *moves, QString *error);
bool isPortablePreviewPath(const QString &path);
QString portablePreviewPath(const QString &asset);
bool verifyPortablePreview(const QString &path, QString *error);

}
