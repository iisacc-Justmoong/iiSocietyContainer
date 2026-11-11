#include "AssetTransfer.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QUuid>

namespace iiSocietyContainer::detail {
namespace {
struct SourceState {
    qint64 size;
    QDateTime modified;
    QDateTime changed;
    bool directory;
};
bool unchanged(const QString &path, const SourceState &state, bool moved) {
    const QFileInfo current(path);
    return !current.isSymLink() && !current.isJunction() && (current.isFile() || current.isDir())
        && current.isDir() == state.directory && current.size() == state.size
        && current.lastModified() == state.modified && (moved || current.metadataChangeTime() == state.changed);
}
}
AssetTransferResult transferAssetGroup(const AssetMoveList &moves, const CopyAsset &copy,
                                       const MoveAssets &move, const RemoveAsset &remove,
                                       const std::function<bool(QString *)> &commit) {
    if (moves.isEmpty()) return {false, true, QStringLiteral("There is no asset to transfer.")};
    QSet<QString> sourceNames, targetNames;
    QSet<QString> sourceParents, targetParents;
    QList<SourceState> initial;
    for (const auto &entry : moves) {
        const QFileInfo source(entry.first), target(entry.second);
        const QFileInfo sourceParent(source.absolutePath()), targetParent(target.absolutePath());
        if (!source.isAbsolute() || !target.isAbsolute()
            || QDir::cleanPath(entry.first) != entry.first || QDir::cleanPath(entry.second) != entry.second
            || !sourceParent.isDir() || sourceParent.isSymLink() || sourceParent.isJunction()
            || sourceParent.canonicalFilePath() != source.absolutePath()
            || !targetParent.isDir() || targetParent.isSymLink() || targetParent.isJunction()
            || targetParent.canonicalFilePath() != target.absolutePath()
            || sourceNames.contains(entry.first) || targetNames.contains(entry.second)
            || source.isSymLink() || source.isJunction() || (!source.isFile() && !source.isDir()))
            return {false, true, QStringLiteral("The asset or its transfer folder is unavailable or redirected.")};
        sourceNames.insert(entry.first); targetNames.insert(entry.second);
        sourceParents.insert(source.absolutePath()); targetParents.insert(target.absolutePath());
        initial.append({source.size(), source.lastModified(), source.metadataChangeTime(), source.isDir()});
    }
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QMap<QString, QString> stages, recoveries;
    const auto cleanup = [&] {
        bool ok = true;
        for (const auto &path : stages) if (QFileInfo::exists(path)) ok = remove(path) && ok;
        for (const auto &path : recoveries) if (QFileInfo::exists(path)) ok = remove(path) && ok;
        return ok;
    };
    for (const auto &parent : targetParents) {
        const auto stage = parent + "/.society-transfer-" + id;
        if (!QDir().mkdir(stage)) { (void)cleanup(); return {false, true, QStringLiteral("Could not reserve a transfer folder.")}; }
        stages.insert(parent, stage);
    }
    for (const auto &parent : sourceParents) {
        const auto recovery = parent + "/.society-recovery-" + id;
        if (!QDir().mkdir(recovery)) { (void)cleanup(); return {false, true, QStringLiteral("Could not reserve a recovery folder.")}; }
        recoveries.insert(parent, recovery);
    }
    const auto stageLocations = stages.values().join(", ");
    const auto recoveryLocations = recoveries.values().join(", ");
    AssetMoveList staged, saved, restore;
    for (const auto &entry : moves) {
        const QFileInfo source(entry.first), target(entry.second);
        staged.append({stages.value(target.absolutePath()) + '/' + target.fileName(), entry.second});
        saved.append({entry.first, recoveries.value(source.absolutePath()) + '/' + source.fileName()});
    }
    for (auto it = saved.crbegin(); it != saved.crend(); ++it) restore.append({it->second, it->first});
    QString error;
    for (qsizetype i = 0; i < moves.size(); ++i) {
        if (copy(moves[i].first, staged[i].first, &error)) continue;
        if (!cleanup()) error += QStringLiteral(" Transfer files remain at %1 or %2.").arg(stageLocations, recoveryLocations);
        return {false, true, error};
    }
    for (qsizetype i = 0; i < moves.size(); ++i) {
        if (unchanged(moves[i].first, initial[i], false)) continue;
        error = QStringLiteral("An asset changed while its companions were being copied. The originals were kept.");
        if (!cleanup()) error += QStringLiteral(" Transfer files remain at %1 or %2.").arg(stageLocations, recoveryLocations);
        return {false, true, error};
    }
    bool rolledBack = true;
    if (!move(saved, &error, &rolledBack, nullptr)) {
        if (rolledBack) {
            if (!cleanup()) error += QStringLiteral(" Transfer files remain at %1 or %2.").arg(stageLocations, recoveryLocations);
        } else error += QStringLiteral(" Original files remain in %1; staged copies remain in %2.").arg(recoveryLocations, stageLocations);
        return {false, rolledBack, error};
    }
    for (qsizetype i = 0; i < saved.size(); ++i) {
        if (unchanged(saved[i].second, initial[i], true)) continue;
        QString rollbackError; bool ignored = true;
        const bool originalsRestored = move(restore, &rollbackError, &ignored, nullptr);
        error = QStringLiteral("An asset changed during transfer.");
        if (originalsRestored) {
            if (!cleanup()) error += QStringLiteral(" Transfer files remain at %1 or %2.").arg(stageLocations, recoveryLocations);
        } else error += QStringLiteral(" Preserve the original files at %1 and staged copies at %2.").arg(recoveryLocations, stageLocations);
        return {false, originalsRestored, error};
    }
    bool stagedRestored = true;
    if (!move(staged, &error, &stagedRestored, nullptr)) {
        QString rollbackError; bool ignored = true;
        const bool originalsRestored = move(restore, &rollbackError, &ignored, nullptr);
        if (stagedRestored && originalsRestored) {
            if (!cleanup()) error += QStringLiteral(" Transfer files remain at %1 or %2.").arg(stageLocations, recoveryLocations);
        } else {
            error += QStringLiteral(" Preserve the source, destination, %1 and %2 before retrying.").arg(stageLocations, recoveryLocations);
        }
        return {false, originalsRestored, error};
    }
    if (!commit(&error)) {
        QString rollbackError; bool ignored = true;
        const bool originalsRestored = move(restore, &rollbackError, &ignored, nullptr);
        // Published files may already be observed or edited. Never delete them
        // on a failed commit; retain all copies for an explicit resolution.
        error += QStringLiteral(" Published copies remain at the requested destinations.");
        if (originalsRestored) {
            if (!cleanup()) error += QStringLiteral(" Transfer folders may remain at %1 or %2.").arg(stageLocations, recoveryLocations);
        } else error += QStringLiteral(" Recovery files remain at %1.").arg(recoveryLocations);
        return {false, originalsRestored, error};
    }
    for (qsizetype i = 0; i < saved.size(); ++i) {
        if (unchanged(saved[i].second, initial[i], true)) continue;
        for (const auto &stage : stages) (void)remove(stage);
        return {true, false, QStringLiteral("The published copy may be outdated because an original changed during transfer. The changed original remains at %1.").arg(recoveryLocations)};
    }
    if (!cleanup())
        return {true, false, QStringLiteral("The transfer completed, but temporary files remain at %1 or %2.").arg(recoveryLocations, stageLocations)};
    return {true, false, {}};
}
}
