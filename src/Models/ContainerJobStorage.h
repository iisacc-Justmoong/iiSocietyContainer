#pragma once

#include <iiSocietyContainerExport.h>
#include <filesystem>
#include <memory>
#include <string_view>

namespace ii::job { class Store; }
namespace iiFileProvider { class TemporaryLease; }

// Container-owned persistent jobs and cross-process execution leases.
namespace iiSocietyContainer {
class IISOCIETYCONTAINER_EXPORT ContainerJobStorage final
{
public:
    explicit ContainerJobStorage(std::filesystem::path directory);
    [[nodiscard]] static std::filesystem::path defaultDirectory();
    [[nodiscard]] std::unique_ptr<ii::job::Store> open(bool create) const;
    [[nodiscard]] std::unique_ptr<iiFileProvider::TemporaryLease>
    tryLease(std::string_view jobId, bool create) const;

private:
    std::filesystem::path m_directory;
    [[nodiscard]] std::filesystem::path directory(bool create) const;
};

}
