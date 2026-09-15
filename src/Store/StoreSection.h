//
// Created by Justmoong on 2026 Sep 07.
//

#pragma once

#include <iiSocietyContainerExport.h>

#include <QtCore/QList>
#include <QtCore/QString>

namespace iiSocietyContainer {

/// Logical section identities; they do not prescribe a physical storage layout.
enum class StoreSection {
    AssetLibrary,
    Deleted,
    Files,
    Forked,
    GenerationHistory,
    Models,
    Published,
    ThinkingSpace,
    Photos
};

/// Returns the supported sections in their declared display order.
[[nodiscard]] IISOCIETYCONTAINER_EXPORT QList<StoreSection> allStoreSections();

/// Returns the section's display name, or an empty string for an unknown value.
[[nodiscard]] IISOCIETYCONTAINER_EXPORT QString storeSectionName(StoreSection section);

/// Stable key used by drive manifests and native system item identifiers.
[[nodiscard]] IISOCIETYCONTAINER_EXPORT QString storeSectionKey(StoreSection section);

} // namespace iiSocietyContainer
