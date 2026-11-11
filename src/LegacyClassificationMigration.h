#pragma once

#include <QStringList>
#include <stop_token>

namespace iiSocietyContainer {
class SocietyDrive;
namespace detail {
// Called only by the storage owner's background reconciliation worker.
// Unresolved documents are preserved with their former address and reported.
QStringList isolateLegacyClassifications(const SocietyDrive &drive, std::stop_token stop, bool quarantineDetached);
}
}
