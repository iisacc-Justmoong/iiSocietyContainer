#pragma once

#include "iiSocietyContainerExport.h"
#include <QList>
#include <QString>
#include <optional>

namespace iiSocietyContainer {
enum class ModelType {
    Checkpoint, Embedding, Hypernetwork, AestheticGradient, LoRA, LyCORIS, DoRA,
    ControlNet, Upscaler, Motion, VAE, TextEncoder, UNet, CLIPVision, Poses,
    Wildcards, Workflows, ComfyUIWorkflows, Detection, VLM, CLIP, LLM, Other
};

IISOCIETYCONTAINER_EXPORT QList<ModelType> allModelTypes();
IISOCIETYCONTAINER_EXPORT QString modelTypeName(ModelType type);
IISOCIETYCONTAINER_EXPORT std::optional<ModelType> modelTypeFromName(const QString &name);
}
