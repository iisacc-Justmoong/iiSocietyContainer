//
// Created by Justmoong on 2026 Sep 07.
//

#include "StoreSection.h"

#include <array>

namespace iiSocietyContainer {
namespace {

struct SectionDefinition {
    StoreSection section;
    const char* name;
    const char* key;
};

constexpr std::array<SectionDefinition, 8> sectionDefinitions{{
    {StoreSection::AssetLibrary, "Asset Library", "asset-library"},
    {StoreSection::Deleted, "Deleted", "deleted"},
    {StoreSection::Files, "Files", "files"},
    {StoreSection::Forked, "Forked", "forked"},
    {StoreSection::GenerationHistory, "Generation History", "generation-history"},
    {StoreSection::Models, "Models", "models"},
    {StoreSection::Published, "Published", "published"},
    {StoreSection::ThinkingSpace, "Thinking Space", "thinking-space"}
}};

} // namespace

QList<StoreSection> allStoreSections()
{
    QList<StoreSection> sections;
    sections.reserve(sectionDefinitions.size());
    for (const auto& definition : sectionDefinitions) {
        sections.append(definition.section);
    }
    return sections;
}

QString storeSectionName(StoreSection section)
{
    for (const auto& definition : sectionDefinitions) {
        if (definition.section == section) {
            return QString::fromLatin1(definition.name);
        }
    }
    return {};
}

QString storeSectionKey(StoreSection section)
{
    for (const auto& definition : sectionDefinitions) {
        if (definition.section == section) {
            return QString::fromLatin1(definition.key);
        }
    }
    return {};
}

} // namespace iiSocietyContainer
