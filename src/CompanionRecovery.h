#pragma once
#include "../SocietyDrive.h"
#include <QJsonObject>
#include <atomic>
#include <stop_token>

namespace iiSocietyContainer::detail {
QJsonObject modelRecoveryAliases(const SocietyDrive &drive, QString *error);
// The caller owns both the managed-repository and Models operation locks.
QStringList repairModelCompanions(const SocietyDrive &drive, const QJsonObject &aliases,
                                  QStringList *errors, const std::atomic_bool *cancelled,
                                  std::stop_token stop = {});
}
