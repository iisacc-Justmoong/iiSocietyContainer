#include "AssetTransfer.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
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
    const auto sourceParent = QFileInfo(moves.first().first).absolutePath();
    const auto targetParent = QFileInfo(moves.first().second).absolutePath();
    QSet<QString> sourceNames, targetNames;
    QList<SourceState> initial;
    for (const auto &entry : moves) {
        const QFileInfo source(entry.first), target(entry.second);
        if (source.absolutePath() != sourceParent || target.absolutePath() != targetParent
            || sourceNames.contains(source.fileName()) || targetNames.contains(target.fileName())
            || source.isSymLink() || source.isJunction() || (!source.isFile() && !source.isDir()))
            return {false, true, QStringLiteral("The asset companions do not share safe transfer folders.")};
        sourceNames.insert(source.fileName()); targetNames.insert(target.fileName());
        initial.append({source.size(), source.lastModified(), source.metadataChangeTime(), source.isDir()});
    }
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto stage = targetParent + "/.society-transfer-" + id;
    const auto recovery = sourceParent + "/.society-recovery-" + id;
    if (!QDir().mkdir(stage)) return {false, true, QStringLiteral("Could not reserve a transfer folder.")};
    if (!QDir().mkdir(recovery)) {
        if (!remove(stage)) return {false, true, QStringLiteral("Could not reserve a recovery folder; the transfer folder remains at %1.").arg(stage)};
        return {false, true, QStringLiteral("Could not reserve a recovery folder.")};
    }
    AssetMoveList staged, saved, restore;
    for (const auto &entry : moves) {
        staged.append({stage + '/' + QFileInfo(entry.second).fileName(), entry.second});
        saved.append({entry.first, recovery + '/' + QFileInfo(entry.first).fileName()});
    }
    for (auto it = saved.crbegin(); it != saved.crend(); ++it) restore.append({it->second, it->first});
    const auto cleanup = [&] {
        const bool first = remove(stage);
        const bool second = remove(recovery);
        return first && second;
    };
    QString error;
    for (qsizetype i = 0; i < moves.size(); ++i) {
        if (copy(moves[i].first, staged[i].first, &error)) continue;
        if (!cleanup()) error += QStringLiteral(" Transfer files remain at %1 or %2.").arg(stage, recovery);
        return {false, true, error};
    }
    for (qsizetype i = 0; i < moves.size(); ++i) {
        if (unchanged(moves[i].first, initial[i], false)) continue;
        error = QStringLiteral("An asset changed while its companions were being copied. The originals were kept.");
        if (!cleanup()) error += QStringLiteral(" Transfer files remain at %1 or %2.").arg(stage, recovery);
        return {false, true, error};
    }
    bool rolledBack = true;
    if (!move(saved, &error, &rolledBack, nullptr)) {
        if (rolledBack) {
            if (!cleanup()) error += QStringLiteral(" Transfer files remain at %1 or %2.").arg(stage, recovery);
        } else error += QStringLiteral(" Original files remain in %1; staged copies remain in %2.").arg(recovery, stage);
        return {false, rolledBack, error};
    }
    for (qsizetype i = 0; i < saved.size(); ++i) {
        if (unchanged(saved[i].second, initial[i], true)) continue;
        QString rollbackError; bool ignored = true;
        const bool originalsRestored = move(restore, &rollbackError, &ignored, nullptr);
        error = QStringLiteral("An asset changed during transfer.");
        if (originalsRestored) {
            if (!cleanup()) error += QStringLiteral(" Transfer files remain at %1 or %2.").arg(stage, recovery);
        } else error += QStringLiteral(" Preserve the original files at %1 and staged copies at %2.").arg(recovery, stage);
        return {false, originalsRestored, error};
    }
    bool stagedRestored = true;
    if (!move(staged, &error, &stagedRestored, nullptr)) {
        QString rollbackError; bool ignored = true;
        const bool originalsRestored = move(restore, &rollbackError, &ignored, nullptr);
        if (stagedRestored && originalsRestored) {
            if (!cleanup()) error += QStringLiteral(" Transfer files remain at %1 or %2.").arg(stage, recovery);
        } else {
            error += QStringLiteral(" Preserve the source, destination, %1 and %2 before retrying.").arg(stage, recovery);
        }
        return {false, originalsRestored, error};
    }
    if (!commit(&error)) {
        QString rollbackError; bool ignored = true;
        const bool originalsRestored = move(restore, &rollbackError, &ignored, nullptr);
        // Published files may already be observed or edited. Never delete them
        // on a failed commit; retain all copies for an explicit resolution.
        error += QStringLiteral(" Published copies remain at %1.").arg(targetParent);
        if (originalsRestored) {
            const bool removedStage = QDir().rmdir(stage);
            const bool removedRecovery = QDir().rmdir(recovery);
            if (!removedStage || !removedRecovery)
                error += QStringLiteral(" Transfer folders may remain at %1 or %2.").arg(stage, recovery);
        } else error += QStringLiteral(" Recovery files remain at %1.").arg(recovery);
        return {false, originalsRestored, error};
    }
    for (qsizetype i = 0; i < saved.size(); ++i) {
        if (unchanged(saved[i].second, initial[i], true)) continue;
        (void)remove(stage);
        return {true, false, QStringLiteral("The published copy may be outdated because an original changed during transfer. The changed original remains at %1.").arg(recovery)};
    }
    const bool removedRecovery = remove(recovery);
    const bool removedStage = remove(stage);
    if (!removedRecovery || !removedStage)
        return {true, false, QStringLiteral("The transfer completed, but temporary files remain at %1 or %2.").arg(recovery, stage)};
    return {true, false, {}};
}
}
