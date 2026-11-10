#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <functional>

namespace iiSocietyContainer::detail {
using AssetMoveList = QList<QPair<QString, QString>>;
struct AssetTransferResult {
    bool committed = false;
    bool originalsRestored = true;
    QString error;
};
using CopyAsset = std::function<bool(const QString &, const QString &, QString *)>;
using MoveAssets = std::function<bool(const AssetMoveList &, QString *, bool *, int *)>;
using RemoveAsset = std::function<bool(const QString &)>;

// Stage a complete group before publishing it across volumes. The callbacks
// are the same filesystem operations used for ordinary asset manipulation.
AssetTransferResult transferAssetGroup(const AssetMoveList &moves, const CopyAsset &copy,
                                       const MoveAssets &move, const RemoveAsset &remove,
                                       const std::function<bool(QString *)> &commit);
}
