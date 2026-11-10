#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace iiSocietyContainer {

// Each caller passes its own sandbox directory. No system provider is registered.
struct MobileContainer final {
    std::filesystem::path root;
    std::string identifier;
};

struct MobileMigration final {
    std::size_t copied = 0;
    std::size_t already_present = 0;
    std::size_t conflicts = 0;
};

class MobileStorage final {
public:
    // Returns an OS-authorized group only; it never invents a shared path.
    [[nodiscard]] static std::optional<std::filesystem::path>
    app_group_root(std::string_view identifier);
    [[nodiscard]] static std::expected<MobileContainer, std::string>
    prepare(const std::filesystem::path& application_storage);

    // Copies into an already validated Society container, without deleting the
    // source. Existing unequal files are preserved under a deterministic name.
    [[nodiscard]] static std::expected<MobileMigration, std::string>
    migrate(const MobileContainer& source, const std::filesystem::path& destination);
};
}
