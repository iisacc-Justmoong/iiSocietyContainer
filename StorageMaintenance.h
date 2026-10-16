#pragma once
#include "iiSocietyContainerExport.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace iiSocietyContainer {
/// Identity-checked housekeeping of SDK-owned temporary files and private logs.
/// This API uses C++23 and native filesystem leases, with no Qt dependency.
class IISOCIETY_MAINTENANCE_EXPORT StorageMaintenance final {
public:
    struct Report {
        std::uint64_t tempRemoved = 0;
        std::uint64_t logsRemoved = 0;
        std::uint64_t cacheRemoved = 0; // Recognized Finder metadata at owned roots.
        std::uint64_t bytesRemoved = 0; // Logical bytes; not an APFS allocation estimate.
        std::uint64_t skippedBusy = 0; // Parent directories with an active lease.
        std::vector<std::string> errors;
    };
    /// Remove abandoned SDK temporaries immediately and keep one newest private
    /// .log per log directory. Active shared leases and public files are kept.
    static Report runOnce(const std::filesystem::path& root, std::string_view identifier);
    /// Register an initial asynchronous scan and recurring scans on one process
    /// worker. Reopening the same root/identity does not create another worker.
    /// The process owns the worker and joins it during normal shutdown.
    static void monitor(const std::filesystem::path& root, std::string identifier,
                        std::chrono::seconds interval = std::chrono::minutes(5));
};
}
