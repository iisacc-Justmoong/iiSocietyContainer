#include "ModelType.h"
#include <array>

namespace iiSocietyContainer {
namespace {
constexpr std::array names{"Checkpoint", "Embedding", "Hypernetwork", "Aesthetic Gradient",
    "LoRA", "LyCORIS", "DoRA", "Controlnet", "Upscaler", "Motion", "VAE", "Text Encoder",
    "UNet", "CLIP Vision", "Poses", "Wildcards", "Workflows", "ComfyUI Workflows",
    "Detection", "VLM", "CLIP", "LLM", "Other"};
QString key(QString value)
{
    return value.toLower().remove(' ').remove('_').remove('-');
}
}
QList<ModelType> allModelTypes()
{
    QList<ModelType> result;
    for (size_t index = 0; index < names.size(); ++index) result.append(ModelType(index));
    return result;
}
QString modelTypeName(ModelType type)
{
    const auto index = size_t(type);
    return QString::fromLatin1(names[index < names.size() ? index : size_t(ModelType::Other)]);
}
std::optional<ModelType> modelTypeFromName(const QString &name)
{
    const auto value = key(name);
    for (const auto type : allModelTypes())
        if (value == key(modelTypeName(type))) return type;
    if (value == "textualinversion" || value == "embeddings") return ModelType::Embedding;
    if (value == "locon" || value == "loha" || value == "lokr") return ModelType::LyCORIS;
    if (value == "animatediff") return ModelType::Motion;
    if (value == "diffusers" || value == "checkpoints") return ModelType::Checkpoint;
    return {};
}
}
