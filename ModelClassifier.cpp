#include "ModelClassifier.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <limits>

namespace iiSocietyContainer {
namespace {
constexpr qint64 maxHeader = 16 * 1024 * 1024;
constexpr qint64 maxJson = 1024 * 1024;
bool plainFile(const QString &path)
{
    const QFileInfo file(path);
    return file.isFile() && file.isReadable() && !file.isSymLink() && !file.isJunction();
}
QJsonObject jsonFile(const QString &path)
{
    QFile file(path);
    if (!plainFile(path) || file.size() > maxJson || !file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.read(maxJson)).object();
}
// QJsonDocument accepts duplicate members. Inspect keys after syntax validation so
// ambiguous tensor descriptors cannot silently replace earlier header entries.
bool duplicateJsonKeys(const QByteArray &json)
{
    QList<QSet<QString>> objects;
    for (qsizetype index = 0; index < json.size(); ++index) {
        const auto ch = json.at(index);
        if (ch == '{') objects.append(QSet<QString>());
        else if (ch == '}') objects.removeLast();
        else if (ch == '"') {
            const auto start = index++;
            while (index < json.size() && json.at(index) != '"') {
                if (json.at(index) == '\\') ++index;
                ++index;
            }
            auto next = index + 1;
            while (next < json.size() && (json.at(next) == ' ' || json.at(next) == '\t'
                    || json.at(next) == '\r' || json.at(next) == '\n')) ++next;
            if (next < json.size() && json.at(next) == ':' && !objects.isEmpty()) {
                const auto quoted = json.mid(start, index - start + 1);
                const auto key = quoted.contains('\\')
                    ? QJsonDocument::fromJson('[' + quoted + ']').array().first().toString()
                    : QString::fromUtf8(quoted.mid(1, quoted.size() - 2));
                if (objects.last().contains(key)) return true;
                objects.last().insert(key);
            }
        }
    }
    return false;
}
bool unsignedInteger(const QJsonValue &value, quint64 &result)
{
    const auto integer = value.toInteger(-1);
    if (!value.isDouble() || integer < 0 || value.toDouble() != double(integer)) return false;
    result = quint64(integer);
    return true;
}
int dtypeBits(const QString &dtype)
{
    // Safetensors' public dtype contract, including byte-aligned packed tensors.
    static const QHash<QString, int> bits{
        {"F4", 4}, {"F6_E2M3", 6}, {"F6_E3M2", 6},
        {"BOOL", 8}, {"I8", 8}, {"U8", 8}, {"F8_E4M3", 8}, {"F8_E5M2", 8},
        {"F8_E8M0", 8}, {"F8_E4M3FNUZ", 8}, {"F8_E5M2FNUZ", 8},
        {"I16", 16}, {"U16", 16}, {"F16", 16}, {"BF16", 16},
        {"I32", 32}, {"U32", 32}, {"F32", 32},
        {"I64", 64}, {"U64", 64}, {"F64", 64}, {"C64", 64}};
    return bits.value(dtype);
}
struct TensorHeader {
    QJsonObject tensors;
    QString error;
};
TensorHeader safetensorsHeader(QFile &file)
{
    const auto prefix = file.read(8);
    if (prefix.size() != 8) return {{}, "truncated safetensors header"};
    const auto length = qFromLittleEndian<quint64>(prefix.constData());
    if (!length || length > quint64(maxHeader) || length > quint64(std::max(qint64(0), file.size() - 8)))
        return {{}, "invalid or oversized safetensors header"};
    const auto json = file.read(qint64(length));
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (quint64(json.size()) != length || !json.startsWith('{') || parseError.error != QJsonParseError::NoError || !document.isObject())
        return {{}, "invalid safetensors JSON header"};
    if (duplicateJsonKeys(json)) return {{}, "duplicate safetensors JSON key"};
    const auto header = document.object();
    const auto payloadSize = quint64(file.size() - 8 - qint64(length));
    QList<QPair<quint64, quint64>> ranges;
    for (auto it = header.begin(); it != header.end(); ++it) {
        if (it.key() == "__metadata__") {
            if (!it.value().isObject()) return {{}, "invalid safetensors metadata object"};
            const auto metadata = it.value().toObject();
            for (const auto &value : metadata)
                if (!value.isString()) return {{}, "non-string safetensors metadata value"};
            continue;
        }
        const auto invalid = [&]() -> TensorHeader { return {{}, "invalid safetensors tensor descriptor: " + it.key()}; };
        if (!it.value().isObject()) return invalid();
        const auto tensor = it.value().toObject();
        const auto bits = dtypeBits(tensor.value("dtype").toString());
        if (!bits || !tensor.value("shape").isArray() || !tensor.value("data_offsets").isArray()) return invalid();
        quint64 elements = 1;
        for (const auto &dimension : tensor.value("shape").toArray()) {
            quint64 size = 0;
            if (!unsignedInteger(dimension, size) || (size && elements > std::numeric_limits<quint64>::max() / size)) return invalid();
            elements *= size;
        }
        if (elements > std::numeric_limits<quint64>::max() / quint64(bits)) return invalid();
        const auto bitCount = elements * quint64(bits);
        const auto offsets = tensor.value("data_offsets").toArray();
        quint64 begin = 0, end = 0;
        if (bitCount % 8 || offsets.size() != 2 || !unsignedInteger(offsets.at(0), begin) || !unsignedInteger(offsets.at(1), end)
            || begin > end || end > payloadSize || end - begin != bitCount / 8) return invalid();
        ranges.append({begin, end});
    }
    if (ranges.isEmpty()) return {{}, "safetensors header contains no tensors"};
    std::sort(ranges.begin(), ranges.end());
    quint64 cursor = 0;
    for (const auto &range : ranges) {
        if (range.first != cursor) return {{}, "overlapping or non-contiguous safetensors tensor data"};
        cursor = range.second;
    }
    if (cursor != payloadSize) return {{}, "unindexed or truncated safetensors tensor data"};
    return {header, {}};
}

ModelClassification known(ModelType type, const QString &evidence) { return {type, true, evidence}; }
std::optional<ModelType> explicitType(const QJsonObject &metadata)
{
    for (const auto *name : {"society.model_type", "modelspec.type", "modelType", "type"})
        if (const auto type = modelTypeFromName(metadata.value(QLatin1String(name)).toString())) return type;
    return modelTypeFromName(metadata.value("model").toObject().value("type").toString());
}
ModelClassification configType(const QJsonObject &config)
{
    if (const auto type = explicitType(config)) return known(*type, "explicit model type metadata");
    QStringList names{config.value("_class_name").toString(), config.value("model_type").toString()};
    for (const auto &entry : config.value("architectures").toArray()) names.append(entry.toString());
    const auto architecture = names.join(' ').toLower();
    const auto has = [&](const char *part) { return architecture.contains(QLatin1String(part)); };
    if (config.value("peft_type").toString().compare("LORA", Qt::CaseInsensitive) == 0)
        return known(config.value("use_dora").toBool() ? ModelType::DoRA : ModelType::LoRA, "PEFT adapter configuration");
    if (has("controlnet")) return known(ModelType::ControlNet, "ControlNet configuration");
    if (has("autoencoder") || has("vae")) return known(ModelType::VAE, "autoencoder configuration");
    if (has("clipvision") || has("clip_vision")) return known(ModelType::CLIPVision, "CLIP vision configuration");
    if (has("cliptext") || has("clip_text") || has("t5encoder") || has("umt5encoder"))
        return known(ModelType::TextEncoder, "text encoder configuration");
    if (has("clipmodel") || architecture.trimmed() == "clip") return known(ModelType::CLIP, "CLIP configuration");
    if (has("llava") || has("qwen2_vl") || has("qwen2_5_vl") || has("qwen3_vl") || has("mllama")
        || (config.contains("vision_config") && config.contains("text_config")))
        return known(ModelType::VLM, "multimodal language configuration");
    if (has("forobjectdetection") || has("detr") || has("yolo")) return known(ModelType::Detection, "detector configuration");
    if (has("rrdb") || has("swinir") || has("esrgan")) return known(ModelType::Upscaler, "upscaler configuration");
    if (has("motionadapter") || has("animatediff")) return known(ModelType::Motion, "motion adapter configuration");
    if (has("unet") || has("transformer2dmodel")) return known(ModelType::UNet, "diffusion network configuration");
    if (has("forcausallm") || has("llama") || has("mistral") || has("gemma") || has("qwen2") || has("qwen3"))
        return known(ModelType::LLM, "language model configuration");
    return {};
}
bool animaCheckpoint(const QJsonObject &header)
{
    // ComfyUI's Anima/Cosmos detector identifies the backbone and LLM adapter.
    // Require the input/output projections too, in one namespace with compatible
    // dimensions. This covers net.*, model.diffusion_model.*, and merged wrappers.
    const QString backbone = "blocks.0.mlp.layer1.weight";
    for (auto it = header.begin(); it != header.end(); ++it) {
        if (!it.key().endsWith(backbone)) continue;
        const auto prefix = it.key().chopped(backbone.size());
        if (!prefix.isEmpty() && !prefix.endsWith('.')) continue;
        const auto matrix = [&](const QString &name) {
            const auto shape = header.value(prefix + name).toObject().value("shape").toArray();
            return shape.size() == 2 && shape[0].toInteger() > 0 && shape[1].toInteger() > 0 ? shape : QJsonArray();
        };
        const auto mlp = matrix(backbone);
        const auto adapter = matrix("llm_adapter.blocks.0.cross_attn.q_proj.weight");
        const auto input = matrix("x_embedder.proj.1.weight");
        const auto output = matrix("final_layer.linear.weight");
        if (!mlp.isEmpty() && !adapter.isEmpty() && !input.isEmpty() && !output.isEmpty()
            && mlp[1] == input[0] && output[1] == input[0] && adapter[0] == adapter[1]) return true;
    }
    return false;
}
ModelClassification tensorType(const QJsonObject &header)
{
    const auto metadata = header.value("__metadata__").toObject();
    if (const auto type = explicitType(metadata)) return known(*type, "safetensors type metadata");
    const auto architecture = metadata.value("modelspec.architecture").toString().toLower();
    const auto network = metadata.value("ss_network_module").toString().toLower();
    const auto keys = header.keys();
    const auto has = [&](const char *part) {
        return std::any_of(keys.cbegin(), keys.cend(), [&](const QString &key) { return key.contains(QLatin1String(part)); });
    };
    if (has("lora_magnitude_vector") || has("dora_scale") || has(".dora_"))
        return known(ModelType::DoRA, "DoRA magnitude tensors");
    if (has("hada_") || has("lokr_") || has("oft_blocks") || has("boft_") || network.contains("lycoris"))
        return known(ModelType::LyCORIS, "LyCORIS tensors or network metadata");
    if (has("lora_A") || has("lora_B") || has("lora_down") || has("lora_up") || has("lora.down") || has("lora.up")
        || network.contains("networks.lora") || architecture.contains("lora"))
        return known(ModelType::LoRA, "LoRA tensors or network metadata");
    if (animaCheckpoint(header)) return known(ModelType::Checkpoint, "Anima diffusion backbone, LLM adapter and compatible projection tensors");
    if (has("model.diffusion_model.") && (has("first_stage_model.") || has("cond_stage_model.") || has("conditioner.")))
        return known(ModelType::Checkpoint, "diffusion checkpoint component tensors");
    if (has("controlnet_cond_embedding.") || has("controlnet_down_blocks.") || has("input_hint_block.") || has("zero_convs."))
        return known(ModelType::ControlNet, "ControlNet conditioning tensors");
    if (has("motion_modules.") || has("temporal_transformer.")) return known(ModelType::Motion, "motion tensors");
    if (has("emb_params") || has("string_to_param") || header.contains("clip_l") || header.contains("clip_g"))
        return known(ModelType::Embedding, "textual inversion embedding tensors");
    if ((has("encoder.conv_in.") && has("decoder.conv_out.")) || (has("quant_conv.") && has("post_quant_conv.")))
        return known(ModelType::VAE, "autoencoder tensors");
    if (has("rdb1.conv1.") || (has("conv_first.") && has("conv_last.") && has("body.")))
        return known(ModelType::Upscaler, "super-resolution network tensors");
    const bool language = has("lm_head.") || has("model.embed_tokens.");
    const bool vision = has("vision_model.") || has("visual.") || has("vision_tower.");
    if (language && (vision || has("multi_modal_projector."))) return known(ModelType::VLM, "language and vision tensors");
    if (vision && has("text_model.")) return known(ModelType::CLIP, "CLIP text and vision tensors");
    if (vision) return known(ModelType::CLIPVision, "vision encoder tensors");
    if (language && has("model.layers.")) return known(ModelType::LLM, "language decoder tensors");
    if (has("text_model.") || has("token_embedding.") || has("encoder.block.0.layer.0.SelfAttention."))
        return known(ModelType::TextEncoder, "text encoder tensors");
    if (has("input_blocks.") || has("down_blocks.") || has("double_blocks.") || has("transformer_blocks."))
        return known(ModelType::UNet, "standalone diffusion network tensors");
    return {};
}

// GGUF metadata is typed and length-prefixed. Stop before tensor data and bound all allocations.
QJsonObject ggufMetadata(QFile &file)
{
    QDataStream input(&file); input.setByteOrder(QDataStream::LittleEndian);
    quint32 version = 0; quint64 tensors = 0, count = 0;
    input >> version >> tensors >> count;
    if ((version != 2 && version != 3) || count > 100000) return {};
    const auto string = [&]() -> QString {
        quint64 size; input >> size;
        if (input.status() != QDataStream::Ok || size > quint64(maxJson) || file.pos() + qint64(size) > std::min(file.size(), maxHeader)) {
            input.setStatus(QDataStream::ReadCorruptData); return {};
        }
        return QString::fromUtf8(file.read(qint64(size)));
    };
    const auto skip = [&](auto &&self, quint32 type, int depth) -> bool {
        if (depth > 1 || file.pos() > maxHeader) return false;
        if (type == 8) { string(); return input.status() == QDataStream::Ok; }
        if (type == 9) {
            quint32 element; quint64 length; input >> element >> length;
            if (length > 1000000 || element == 9) return false;
            for (quint64 index = 0; index < length; ++index) if (!self(self, element, depth + 1)) return false;
            return true;
        }
        const int size = type <= 1 || type == 7 ? 1 : type <= 3 ? 2 : type <= 6 ? 4 : type >= 10 && type <= 12 ? 8 : 0;
        return size && file.pos() + size <= std::min(file.size(), maxHeader) && file.seek(file.pos() + size);
    };
    QJsonObject result;
    for (quint64 index = 0; index < count && file.pos() < maxHeader; ++index) {
        const auto key = string(); quint32 type; input >> type;
        if (input.status() != QDataStream::Ok) return {};
        if (type == 8) {
            const auto value = string();
            if (key == "general.architecture" || key == "general.name") result.insert(key, value);
        } else if (type == 4 && key == "general.file_type") {
            quint32 value; input >> value; result.insert(key, int(value));
        } else if (type == 7 && (key == "clip.has_text_encoder" || key == "clip.has_vision_encoder")) {
            quint8 value; input >> value; result.insert(key, value != 0);
        } else if (!skip(skip, type, 0)) return {};
    }
    return input.status() == QDataStream::Ok ? result : QJsonObject();
}
}

QStringList ModelClassifier::companionFiles(const QString &path)
{
    const QFileInfo file(path);
    if (!file.isFile()) return {};
    const auto stem = QDir(file.absolutePath()).filePath(file.completeBaseName());
    QStringList result;
    for (const auto &prefix : {path, stem}) {
        for (const auto *suffix : {".model.json", ".civitai.info", ".preview.png", ".preview.jpg", ".preview.webp"}) {
            const auto candidate = prefix + QLatin1String(suffix);
            if (candidate != path && plainFile(candidate) && !result.contains(candidate)) result.append(candidate);
        }
    }
    return result;
}
QJsonObject ModelClassifier::metadata(const QString &path)
{
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isAbsolute() || !info.isReadable() || info.isSymLink() || info.isJunction()) return {};
    QJsonObject result;
    const auto merge = [&](const QJsonObject &values) {
        for (auto it = values.begin(); it != values.end(); ++it) result.insert(it.key(), it.value());
    };
    if (info.isDir()) {
        const QDir directory(path);
        for (const auto *name : {"config.json", "model_index.json", "adapter_config.json"})
            merge(jsonFile(directory.filePath(QLatin1String(name))));
        merge(jsonFile(directory.filePath("society.model.json")));
        return result;
    }
    QFile file(path);
    if (!plainFile(path) || !file.open(QIODevice::ReadOnly)) return {};
    const auto suffix = info.suffix().toLower();
    if (suffix == "safetensors" || suffix == "safetensor") {
        const auto inspected = safetensorsHeader(file);
        if (!inspected.error.isEmpty()) return {};
        const auto &header = inspected.tensors;
        merge(header.value("__metadata__").toObject());
        QStringList dtypes;
        for (auto it = header.begin(); it != header.end(); ++it) {
            const auto dtype = it.value().toObject().value("dtype").toString();
            if (it.key() != "__metadata__" && !dtype.isEmpty() && !dtypes.contains(dtype)) dtypes.append(dtype);
        }
        dtypes.sort();
        if (!dtypes.isEmpty()) result.insert("tensor_dtypes", QJsonArray::fromStringList(dtypes));
    } else if (suffix == "gguf" && file.read(4) == "GGUF") {
        merge(ggufMetadata(file));
    }
    for (const auto &companion : companionFiles(path)) {
        if (companion.endsWith(".model.json") || companion.endsWith(".civitai.info")) merge(jsonFile(companion));
    }
    return result;
}
bool ModelClassifier::isPackage(const QString &directory)
{
    const QFileInfo info(directory);
    if (!info.isDir() || info.isSymLink() || info.isJunction()) return false;
    const QDir folder(directory);
    if (plainFile(folder.filePath("model_index.json")) || plainFile(folder.filePath("adapter_config.json"))) return true;
    return plainFile(folder.filePath("config.json"))
        && !folder.entryList({"*.safetensors", "*.safetensor", "*.bin", "*.gguf"}, QDir::Files | QDir::NoSymLinks).isEmpty();
}
ModelClassification ModelClassifier::classify(const QString &path, const QString &fileName)
{
    const QFileInfo info(path);
    if (info.isSymLink() || info.isJunction()) return {ModelType::Other, false, "redirected input"};
    const auto suffix = QFileInfo(fileName.isEmpty() ? path : fileName).suffix().toLower();
    QJsonObject tensors;
    if (!info.isDir() && (suffix == "safetensors" || suffix == "safetensor")) {
        QFile file(path);
        if (!plainFile(path) || !file.open(QIODevice::ReadOnly)) return {ModelType::Other, false, "unreadable input"};
        const auto inspected = safetensorsHeader(file);
        if (!inspected.error.isEmpty()) return {ModelType::Other, false, inspected.error};
        tensors = inspected.tensors;
    }
    for (const auto &companion : companionFiles(path)) {
        if (const auto type = explicitType(jsonFile(companion))) return known(*type, "model sidecar metadata");
    }
    if (info.isDir()) {
        const QDir folder(path);
        const auto adapter = configType(jsonFile(folder.filePath("adapter_config.json")));
        if (adapter.recognized) return adapter;
        if (plainFile(folder.filePath("model_index.json"))) return known(ModelType::Checkpoint, "Diffusers pipeline package");
        return configType(jsonFile(folder.filePath("config.json")));
    }
    QFile file(path);
    if (!plainFile(path) || !file.open(QIODevice::ReadOnly)) return {ModelType::Other, false, "unreadable input"};
    if (suffix == "safetensors" || suffix == "safetensor") {
        const auto result = tensorType(tensors);
        if (result.recognized) return result;
    } else if (suffix == "gguf" && file.read(4) == "GGUF") {
        const auto metadata = ggufMetadata(file);
        const auto architecture = metadata.value("general.architecture").toString().toLower();
        if (architecture == "clip") {
            const bool text = metadata.value("clip.has_text_encoder").toBool();
            const bool vision = metadata.value("clip.has_vision_encoder").toBool();
            return known(text && vision ? ModelType::CLIP : text ? ModelType::TextEncoder : ModelType::CLIPVision, "GGUF CLIP architecture");
        }
        if (architecture.contains("flux") || architecture.contains("stable-diffusion")) return known(ModelType::UNet, "GGUF diffusion architecture");
        if (architecture == "t5" || architecture == "t5encoder") return known(ModelType::TextEncoder, "GGUF text encoder architecture");
        if (architecture.contains("vl") || architecture == "mllama") return known(ModelType::VLM, "GGUF multimodal architecture");
        const QStringList language{"llama", "mistral", "qwen", "qwen2", "qwen3", "qwen3moe", "gemma", "gemma2", "gemma3", "phi2", "phi3", "gpt2", "falcon", "deepseek2"};
        if (language.contains(architecture)) return known(ModelType::LLM, "GGUF language architecture");
    } else if (suffix == "json" && file.size() <= maxJson) {
        const auto object = QJsonDocument::fromJson(file.readAll()).object();
        const auto configured = configType(object);
        if (configured.recognized) return configured;
        if (object.value("nodes").isArray() && object.value("links").isArray()) return known(ModelType::ComfyUIWorkflows, "ComfyUI workflow graph");
        const auto prompt = object.value("prompt").isObject() ? object.value("prompt").toObject() : object;
        if (!prompt.isEmpty() && std::all_of(prompt.begin(), prompt.end(), [](const QJsonValue &node) {
                return node.isObject() && node.toObject().value("class_type").isString() && node.toObject().value("inputs").isObject();
            })) return known(ModelType::ComfyUIWorkflows, "ComfyUI API prompt graph");
        const auto people = object.value("people").toArray();
        if (!people.isEmpty() && people.first().toObject().value("pose_keypoints_2d").isArray()) return known(ModelType::Poses, "OpenPose keypoints");
        if (object.contains("workflow") || object.value("steps").isArray()) return known(ModelType::Workflows, "workflow document");
    } else if ((suffix == "txt" || suffix == "wildcards") && file.size() > 0 && file.size() <= maxJson
        && !info.completeBaseName().startsWith("readme", Qt::CaseInsensitive) && !file.peek(maxJson).contains('\0')) {
        return known(ModelType::Wildcards, "plain-text wildcard list");
    }
    return {ModelType::Other, false, "insufficient model metadata"};
}
}
