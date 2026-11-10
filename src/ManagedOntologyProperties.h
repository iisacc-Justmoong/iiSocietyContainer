#pragma once
#include <array>
#include <string_view>

namespace iiSocietyContainer::detail {

// The file-intake worker may finish during process shutdown. Keep these names
// in trivially destructible static storage rather than a destructed tree.
constexpr bool managedOntologyProperty(std::string_view key) noexcept
{
    constexpr std::array<std::string_view, 18> names{
        "society:original", "society:catalogKey", "society:section", "society:name",
        "society:title", "society:color", "society:rating", "society:mediaType",
        "society:keyword", "society:collection", "society:resourceId", "society:contentRevision",
        "society:contentSha256", "society:originalSha256", "society:recordValidation", "society:location", "society:containerId",
        "society:entitlementSha256"};
    for (const auto name : names) if (name == key) return true;
    return false;
}
}
