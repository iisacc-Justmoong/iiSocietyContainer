#include "ModelStore.h"
#include "SharedStorage.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <algorithm>

using namespace iiSocietyContainer;
namespace {
bool write(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path)
{
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QByteArray tensorFile(const QJsonObject &header, const QByteArray &data)
{
    const auto json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray result(8, '\0'); qToLittleEndian(quint64(json.size()), result.data());
    return result + json + data;
}
QJsonObject animaTensors(const QString &prefix)
{
    // Reduced dimensions; tensor names and their dimension relationships match Anima.
    QJsonObject header;
    int offset = 0;
    const auto add = [&](const QString &name, const QJsonArray &shape) {
        int bytes = 4;
        for (const auto &dimension : shape) bytes *= dimension.toInt();
        header.insert(prefix + name, QJsonObject{{"dtype", "F32"}, {"shape", shape},
            {"data_offsets", QJsonArray{offset, offset + bytes}}});
        offset += bytes;
    };
    add("blocks.0.mlp.layer1.weight", {32, 8});
    add("llm_adapter.blocks.0.cross_attn.q_proj.weight", {4, 4});
    add("x_embedder.proj.1.weight", {8, 68});
    add("final_layer.linear.weight", {64, 8});
    return header;
}
QByteArray tensorFile(const QJsonObject &header)
{
    int bytes = 0;
    for (const auto &tensor : header)
        bytes = std::max(bytes, tensor.toObject().value("data_offsets").toArray().last().toInt());
    return tensorFile(header, QByteArray(bytes, '\0'));
}
QByteArray weights(const QStringList &keys, const QJsonObject &metadata = {})
{
    QJsonObject header;
    int offset = 0;
    for (const auto &key : keys) {
        header.insert(key, QJsonObject{{"dtype", "F32"}, {"shape", QJsonArray{1}}, {"data_offsets", QJsonArray{offset, offset + 4}}});
        offset += 4;
    }
    if (!metadata.isEmpty()) header.insert("__metadata__", metadata);
    const auto json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray result(8, '\0'); qToLittleEndian(quint64(json.size()), result.data());
    return result + json + QByteArray(offset, '\0');
}
QByteArray gguf(const QString &architecture, bool text = false, bool vision = false)
{
    QByteArray result("GGUF"); QDataStream stream(&result, QIODevice::Append); stream.setByteOrder(QDataStream::LittleEndian);
    const auto string = [&](const QByteArray &value) { stream << quint64(value.size()); stream.writeRawData(value.data(), value.size()); };
    stream << quint32(3) << quint64(0) << quint64(3);
    string("general.architecture"); stream << quint32(8); string(architecture.toUtf8());
    string("clip.has_text_encoder"); stream << quint32(7) << quint8(text);
    string("clip.has_vision_encoder"); stream << quint32(7) << quint8(vision);
    return result;
}
}

class ModelStoreTests : public QObject {
    Q_OBJECT
private slots:
    void exposesBoundedModelMetadataForCatalogs()
    {
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/catalog-metadata-XXXXXX"); QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("model.safetensors");
        QVERIFY(write(path, weights({"tensor"}, {{"modelspec.architecture", "stable-diffusion-xl-v1-base"}})));
        auto metadata = ModelClassifier::metadata(path);
        QCOMPARE(metadata.value("modelspec.architecture").toString(), QString("stable-diffusion-xl-v1-base"));
        QCOMPARE(metadata.value("tensor_dtypes").toArray(), QJsonArray{"F32"});
        QVERIFY(write(path + ".model.json", "{\"society.modality\":\"video\"}"));
        QCOMPARE(ModelClassifier::metadata(path).value("society.modality").toString(), QString("video"));
        QVERIFY(write(fixture.filePath("voice/config.json"), "{\"architectures\":[\"WhisperForConditionalGeneration\"],\"torch_dtype\":\"float16\"}"));
        QCOMPARE(ModelClassifier::metadata(fixture.filePath("voice")).value("torch_dtype").toString(), QString("float16"));
        QVERIFY(write(fixture.filePath("language.gguf"), gguf("llama")));
        QCOMPARE(ModelClassifier::metadata(fixture.filePath("language.gguf")).value("general.architecture").toString(), QString("llama"));
        QVERIFY(write(fixture.filePath("invalid.safetensors"), QByteArray::fromHex("ffffffffffffffff")));
        QVERIFY(ModelClassifier::metadata(fixture.filePath("invalid.safetensors")).isEmpty());
        QVERIFY(QFile::link(path, fixture.filePath("linked.safetensors")));
        QVERIFY(ModelClassifier::metadata(fixture.filePath("linked.safetensors")).isEmpty());
    }

    void createsAllTypesAndPreservesLegacyManifest()
    {
        const QStringList expected{"Checkpoint", "Embedding", "Hypernetwork", "Aesthetic Gradient", "LoRA", "LyCORIS", "DoRA",
            "Controlnet", "Upscaler", "Motion", "VAE", "Text Encoder", "UNet", "CLIP Vision", "Poses", "Wildcards",
            "Workflows", "ComfyUI Workflows", "Detection", "VLM", "CLIP", "LLM", "Other"};
        QCOMPARE(allModelTypes().size(), expected.size());
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/types-XXXXXX"); QVERIFY(fixture.isValid());
        QString error;
        const auto drive = SocietyDrive::create(fixture.path(), &error); QVERIFY2(drive, qPrintable(error));
        const auto store = ModelStore::open(fixture.path(), &error); QVERIFY2(store, qPrintable(error));
        for (qsizetype index = 0; index < expected.size(); ++index) {
            const auto type = allModelTypes()[index];
            QCOMPARE(modelTypeName(type), expected[index]);
            QCOMPARE(modelTypeFromName(expected[index].toLower()), std::optional(type));
            QCOMPARE(store->categoryPath(type), fixture.filePath("Models/" + expected[index]));
            QVERIFY(QFileInfo(store->categoryPath(type)).isDir());
            QVERIFY(QDir().rmdir(store->categoryPath(type))); // Simulate a pre-0.11 Models area.
        }
        const auto manifest = read(fixture.filePath(".society-drive.json"));
        QVERIFY(SocietyDrive::open(fixture.path()));
        QVERIFY(QDir(fixture.filePath("Models")).isEmpty()); // Opening remains read-only.
        QVERIFY(store->ensureLayout(&error));
        QVERIFY(store->ensureLayout(&error));
        QCOMPARE(read(fixture.filePath(".society-drive.json")), manifest);
        QCOMPARE(QDir(fixture.filePath("Models")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 23);
        QVERIFY(!modelTypeFromName("../Checkpoint"));
    }

    void explicitMetadataCoversEveryType()
    {
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/metadata-XXXXXX"); QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("arbitrary.safetensors");
        for (const auto type : allModelTypes()) {
            QVERIFY(write(path, weights({"tensor"}, {{"society.model_type", modelTypeName(type)}})));
            const auto found = ModelClassifier::classify(path);
            QCOMPARE(found.type, type); QVERIFY(found.recognized); QVERIFY(!found.evidence.isEmpty());
        }
        const auto legacy = fixture.filePath("arbitrary.pt");
        QVERIFY(write(legacy, "legacy data"));
        QVERIFY(write(fixture.filePath("arbitrary.civitai.info"), "{\"model\":{\"type\":\"Hypernetwork\"}}"));
        QCOMPARE(ModelClassifier::classify(legacy).type, ModelType::Hypernetwork);
        QCOMPARE(ModelClassifier::companionFiles(legacy), QStringList{fixture.filePath("arbitrary.civitai.info")});
    }

    void animaCheckpoint_data()
    {
        QTest::addColumn<QString>("prefix");
        QTest::newRow("unwrapped") << QString();
        QTest::newRow("base-net") << QString("net.");
        QTest::newRow("finetune") << QString("model.diffusion_model.");
        QTest::newRow("complete") << QString("model.diffusion_model.net.");
    }
    void animaCheckpoint()
    {
        QFETCH(QString, prefix);
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/anima-XXXXXX"); QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("unrelated-name.safetensors");
        auto header = animaTensors(prefix);
        if (prefix == "model.diffusion_model.net.") {
            int offset = tensorFile(header).size() - QJsonDocument(header).toJson(QJsonDocument::Compact).size() - 8;
            for (const auto *key : {"text_encoders.llm.model.embed_tokens.weight", "text_encoders.llm.model.layers.0.self_attn.q_proj.weight",
                    "vae.encoder.conv_in.weight", "vae.decoder.conv_out.weight"}) {
                header.insert(QLatin1String(key), QJsonObject{{"dtype", "F32"}, {"shape", QJsonArray{1}},
                    {"data_offsets", QJsonArray{offset, offset + 4}}});
                offset += 4;
            }
        }
        const auto bytes = tensorFile(header); QVERIFY(write(path, bytes));
        const auto found = ModelClassifier::classify(path);
        QCOMPARE(found.type, ModelType::Checkpoint); QVERIFY(found.recognized);
        QVERIFY(found.evidence.contains("Anima"));
        QCOMPARE(read(path), bytes);
    }

    void animaRequiresConsistentBackboneAndAdapter()
    {
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/anima-evidence-XXXXXX"); QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("anima_checkpoint.safetensors");
        QVERIFY(write(path, weights({"net.blocks.0.mlp.layer1.weight", "net.llm_adapter.blocks.0.cross_attn.q_proj.weight"})));
        QCOMPARE(ModelClassifier::classify(path).type, ModelType::Other);
        auto header = animaTensors("net.");
        header.insert("unrelated.final_layer.linear.weight", header.take("net.final_layer.linear.weight"));
        QVERIFY(write(path, tensorFile(header)));
        QCOMPARE(ModelClassifier::classify(path).type, ModelType::Other);
        header = animaTensors("net.");
        auto projection = header.value("net.x_embedder.proj.1.weight").toObject();
        projection.insert("shape", QJsonArray{16, 34}); // Same byte count, incompatible hidden dimension.
        header.insert("net.x_embedder.proj.1.weight", projection);
        QVERIFY(write(path, tensorFile(header)));
        QCOMPARE(ModelClassifier::classify(path).type, ModelType::Other);
        QVERIFY(write(path, weights({"net.blocks.0.mlp.layer1.lora_A.weight", "net.blocks.0.mlp.layer1.lora_B.weight"},
            {{"modelspec.architecture", "anima"}})));
        QCOMPARE(ModelClassifier::classify(path).type, ModelType::LoRA);
    }

    void validatesTensorDescriptorsAndPayload_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        const QJsonObject tensor{{"dtype", "F32"}, {"shape", QJsonArray{1}}, {"data_offsets", QJsonArray{0, 4}}};
        const auto row = [&](const char *name, const char *field, const QJsonValue &value) {
            auto invalid = tensor; invalid.insert(QLatin1String(field), value);
            QTest::newRow(name) << tensorFile(QJsonObject{{"layer.lora_A.weight", invalid}}, QByteArray(4, '\0'));
        };
        row("unknown-dtype", "dtype", "garbage");
        row("missing-dtype", "dtype", QJsonValue());
        row("negative-shape", "shape", QJsonArray{-1});
        row("fractional-shape", "shape", QJsonArray{1.5});
        row("overflow-shape", "shape", QJsonArray{1e19, 1e19});
        row("missing-shape", "shape", QJsonValue());
        row("wrong-shape-length", "shape", QJsonArray{2});
        row("negative-offset", "data_offsets", QJsonArray{-4, 0});
        row("fractional-offset", "data_offsets", QJsonArray{0, 3.5});
        row("reversed-offset", "data_offsets", QJsonArray{4, 0});
        row("outside-payload", "data_offsets", QJsonArray{4, 8});
        row("missing-offset", "data_offsets", QJsonArray{0});
        QTest::newRow("invalid-descriptor") << tensorFile({{"layer.lora_A.weight", "not a tensor"}}, QByteArray(4, '\0'));
        QTest::newRow("truncated-data") << tensorFile({{"layer.lora_A.weight", tensor}}, QByteArray(3, '\0'));
        QTest::newRow("unindexed-data") << tensorFile({{"layer.lora_A.weight", tensor}}, QByteArray(5, '\0'));
        QTest::newRow("overlapping-data") << tensorFile({{"layer.lora_A.weight", tensor}, {"layer.lora_B.weight", tensor}}, QByteArray(4, '\0'));
        auto later = tensor; later.insert("data_offsets", QJsonArray{8, 12});
        QTest::newRow("gap-in-data") << tensorFile({{"layer.lora_A.weight", tensor}, {"layer.lora_B.weight", later}}, QByteArray(12, '\0'));
        QTest::newRow("non-string-metadata") << tensorFile({{"layer.lora_A.weight", tensor}, {"__metadata__", QJsonObject{{"count", 1}}}}, QByteArray(4, '\0'));
        QTest::newRow("invalid-metadata") << tensorFile({{"layer.lora_A.weight", tensor}, {"__metadata__", "LoRA"}}, QByteArray(4, '\0'));
        QTest::newRow("metadata-without-tensors") << tensorFile({{"__metadata__", QJsonObject{{"society.model_type", "Checkpoint"}}}}, {});
        const auto descriptor = QJsonDocument(tensor).toJson(QJsonDocument::Compact);
        const QList<QPair<const char *, QByteArray>> duplicates{
            {"duplicate-tensor", QByteArray("{\"layer.lora_A.weight\":") + descriptor + ",\"layer.lora_A.weight\":" + descriptor + '}'},
            {"duplicate-field", "{\"layer.lora_A.weight\":{\"dtype\":\"F32\",\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]}}"},
            {"escaped-duplicate-field", "{\"layer.lora_A.weight\":{\"dtype\":\"F32\",\"dtyp\\u0065\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]}}"}};
        for (const auto &[name, json] : duplicates) {
            QByteArray bytes(8, '\0'); qToLittleEndian(quint64(json.size()), bytes.data());
            QTest::newRow(name) << bytes + json + QByteArray(4, '\0');
        }
    }
    void validatesTensorDescriptorsAndPayload()
    {
        QFETCH(QByteArray, bytes);
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/tensor-validation-XXXXXX"); QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("checkpoint.safetensors"); QVERIFY(write(path, bytes));
        const auto result = ModelClassifier::classify(path);
        QVERIFY2(!result.recognized, qPrintable(result.evidence));
        QCOMPARE(result.type, ModelType::Other); QVERIFY(!result.evidence.isEmpty());
        QVERIFY(ModelClassifier::metadata(path).isEmpty());
        QVERIFY(write(path + ".model.json", "{\"type\":\"Checkpoint\"}"));
        QVERIFY(!ModelClassifier::classify(path).recognized); // Sidecars cannot legitimize broken tensor files.
        QCOMPARE(read(path), bytes);
    }

    void acceptsScalarEmptyAndPackedTensors()
    {
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/valid-tensors-XXXXXX"); QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("model.safetensors");
        const QJsonObject header{
            {"layer.lora_A.weight", QJsonObject{{"dtype", "F32"}, {"shape", QJsonArray{}}, {"data_offsets", QJsonArray{0, 4}}}},
            {"empty", QJsonObject{{"dtype", "BF16"}, {"shape", QJsonArray{0, 8}}, {"data_offsets", QJsonArray{4, 4}}}},
            {"packed", QJsonObject{{"dtype", "F4"}, {"shape", QJsonArray{2}}, {"data_offsets", QJsonArray{4, 5}}}}};
        QVERIFY(write(path, tensorFile(header, QByteArray(5, '\0'))));
        QCOMPARE(ModelClassifier::classify(path).type, ModelType::LoRA);
        QCOMPARE(ModelClassifier::metadata(path).value("tensor_dtypes").toArray(), (QJsonArray{"BF16", "F32", "F4"}));
        const auto temporary = fixture.filePath("provider.tmp");
        QVERIFY(QFile::rename(path, temporary));
        QCOMPARE(ModelClassifier::classify(temporary, "original.SAFETENSORS").type, ModelType::LoRA);
    }

    void reorganizesPreviouslyUnknownAnimaAndPreservesReferences()
    {
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/anima-organize-XXXXXX"); QVERIFY(fixture.isValid());
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto source = fixture.filePath("Models/Other/renamed.safetensors");
        const auto bytes = tensorFile(animaTensors("net.")); QVERIFY(write(source, bytes));
        QVERIFY(write(source + ".preview.png", "preview"));
        const auto shared = SharedStorage::open(fixture.path()); QVERIFY(shared);
        const auto reference = shared->models().first().reference(shared->drive().identifier());
        const auto store = ModelStore::open(fixture.path()); QVERIFY(store);
        const auto report = store->organize();
        QVERIFY2(report.errors.isEmpty(), qPrintable(report.errors.join('\n'))); QCOMPARE(report.moved.size(), 1);
        const auto destination = fixture.filePath("Models/Checkpoint/renamed.safetensors");
        QCOMPARE(report.moved.first().path, destination); QCOMPARE(read(destination), bytes);
        QCOMPARE(read(destination + ".preview.png"), QByteArray("preview"));
        QCOMPARE(store->resolve("Other/renamed.safetensors"), destination);
        QCOMPARE(shared->resolveModel(reference), destination);
        QVERIFY(store->organize().moved.isEmpty());
    }

    void tensorSignatures_data()
    {
        QTest::addColumn<QStringList>("keys"); QTest::addColumn<int>("type");
        const auto row = [](const char *name, const QStringList &keys, ModelType type) { QTest::newRow(name) << keys << int(type); };
        row("checkpoint", {"model.diffusion_model.input_blocks.0.weight", "first_stage_model.encoder.conv_in.weight"}, ModelType::Checkpoint);
        row("lora", {"layer.lora_A.weight", "layer.lora_B.weight"}, ModelType::LoRA);
        row("lycoris", {"layer.hada_w1_a", "layer.hada_w1_b"}, ModelType::LyCORIS);
        row("dora-before-lora", {"layer.lora_A.weight", "layer.lora_magnitude_vector.weight"}, ModelType::DoRA);
        row("controlnet", {"controlnet_cond_embedding.conv_in.weight", "controlnet_down_blocks.0.weight"}, ModelType::ControlNet);
        row("motion", {"down_blocks.0.motion_modules.0.weight"}, ModelType::Motion);
        row("embedding", {"emb_params"}, ModelType::Embedding);
        row("vae", {"encoder.conv_in.weight", "decoder.conv_out.weight"}, ModelType::VAE);
        row("upscaler", {"body.0.rdb1.conv1.weight"}, ModelType::Upscaler);
        row("unet", {"double_blocks.0.img_attn.qkv.weight"}, ModelType::UNet);
        row("text-encoder", {"text_model.embeddings.token_embedding.weight"}, ModelType::TextEncoder);
        row("clip-vision", {"vision_model.embeddings.patch_embedding.weight"}, ModelType::CLIPVision);
        row("clip", {"vision_model.embeddings.weight", "text_model.embeddings.weight"}, ModelType::CLIP);
        row("llm", {"model.embed_tokens.weight", "model.layers.0.self_attn.q_proj.weight"}, ModelType::LLM);
        row("vlm", {"model.embed_tokens.weight", "vision_tower.embeddings.weight"}, ModelType::VLM);
        row("unknown-is-not-a-checkpoint", {"layer.weight"}, ModelType::Other);
    }
    void tensorSignatures()
    {
        QFETCH(QStringList, keys); QFETCH(int, type);
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/tensors-XXXXXX"); QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("unrelated name.safetensors");
        const auto bytes = weights(keys); QVERIFY(write(path, bytes));
        QCOMPARE(int(ModelClassifier::classify(path).type), type);
        QCOMPARE(read(path), bytes);
    }

    void packagesDocumentsAndGguf()
    {
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/formats-XXXXXX"); QVERIFY(fixture.isValid());
        const auto pipeline = fixture.filePath("pipeline");
        QVERIFY(write(pipeline + "/model_index.json", "{\"_class_name\":\"StableDiffusionPipeline\"}"));
        QVERIFY(ModelClassifier::isPackage(pipeline));
        QCOMPARE(ModelClassifier::classify(pipeline).type, ModelType::Checkpoint);
        QVERIFY(write(fixture.filePath("adapter/adapter_config.json"), "{\"peft_type\":\"LORA\",\"use_dora\":true}"));
        QCOMPARE(ModelClassifier::classify(fixture.filePath("adapter")).type, ModelType::DoRA);
        const auto config = fixture.filePath("component/config.json");
        QVERIFY(write(fixture.filePath("component/model.safetensors"), weights({"tensor"})));
        for (const auto &[name, type] : QList<QPair<QString, ModelType>>{{"ControlNetModel", ModelType::ControlNet},
                {"AutoencoderKL", ModelType::VAE}, {"CLIPVisionModelWithProjection", ModelType::CLIPVision},
                {"T5EncoderModel", ModelType::TextEncoder}, {"CLIPModel", ModelType::CLIP},
                {"LlamaForCausalLM", ModelType::LLM}, {"LlavaForConditionalGeneration", ModelType::VLM},
                {"RTDetrForObjectDetection", ModelType::Detection}, {"RRDBNet", ModelType::Upscaler},
                {"MotionAdapter", ModelType::Motion}, {"UNet2DConditionModel", ModelType::UNet}}) {
            QVERIFY(write(config, QJsonDocument(QJsonObject{{"architectures", QJsonArray{name}}}).toJson()));
            QVERIFY(ModelClassifier::isPackage(fixture.filePath("component")));
            QCOMPARE(ModelClassifier::classify(fixture.filePath("component")).type, type);
        }
        for (const auto &[json, type] : QList<QPair<QByteArray, ModelType>>{
                {"{\"nodes\":[],\"links\":[]}", ModelType::ComfyUIWorkflows},
                {"{\"1\":{\"class_type\":\"KSampler\",\"inputs\":{}}}", ModelType::ComfyUIWorkflows},
                {"{\"people\":[{\"pose_keypoints_2d\":[1,2,3]}]}", ModelType::Poses},
                {"{\"workflow\":{\"steps\":[]}}", ModelType::Workflows}}) {
            const auto path = fixture.filePath("asset.json"); QVERIFY(write(path, json));
            QCOMPARE(ModelClassifier::classify(path).type, type);
        }
        QVERIFY(write(fixture.filePath("colors.txt"), "red\nblue\ngreen\n"));
        QCOMPARE(ModelClassifier::classify(fixture.filePath("colors.txt")).type, ModelType::Wildcards);
        const auto path = fixture.filePath("model.gguf");
        for (const auto &[name, type] : QList<QPair<QString, ModelType>>{{"llama", ModelType::LLM}, {"qwen2vl", ModelType::VLM},
                {"flux", ModelType::UNet}, {"t5encoder", ModelType::TextEncoder}, {"unknown-architecture", ModelType::Other}}) {
            QVERIFY(write(path, gguf(name))); QCOMPARE(ModelClassifier::classify(path).type, type);
        }
        QVERIFY(write(path, gguf("clip", false, true))); QCOMPARE(ModelClassifier::classify(path).type, ModelType::CLIPVision);
        QVERIFY(write(path, gguf("clip", true, true))); QCOMPARE(ModelClassifier::classify(path).type, ModelType::CLIP);
    }

    void boundedInspectionDoesNotGuessFromNamesOrExecuteLegacyFiles()
    {
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/invalid-XXXXXX"); QVERIFY(fixture.isValid());
        QByteArray oversized(8, '\0'); qToLittleEndian(quint64(256 * 1024 * 1024), oversized.data());
        for (const auto &bytes : {QByteArray(), QByteArray("not a tensor file"), oversized,
                QByteArray::fromHex("1000000000000000") + QByteArray("{}")} ) {
            const auto path = fixture.filePath("checkpoint_lora_vae.safetensors"); QVERIFY(write(path, bytes));
            const auto result = ModelClassifier::classify(path);
            QVERIFY(!result.recognized); QCOMPARE(result.type, ModelType::Other); QCOMPARE(read(path), bytes);
        }
        const auto legacy = fixture.filePath("Llama.pt"); QVERIFY(write(legacy, "__reduce__ execute_me"));
        QCOMPARE(ModelClassifier::classify(legacy).type, ModelType::Other);
        QVERIFY(write(fixture.filePath("corrupt.gguf"), "GGUF"));
        QCOMPARE(ModelClassifier::classify(fixture.filePath("corrupt.gguf")).type, ModelType::Other);
    }

    void organizesPackagesAndCompanionsWithoutReplacingFilesOrReferences()
    {
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/organize-XXXXXX"); QVERIFY(fixture.isValid());
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto base = fixture.filePath("Models");
        const auto source = base + "/Collection/model.safetensors";
        const auto bytes = weights({"model.diffusion_model.input_blocks.0.weight", "first_stage_model.encoder.weight"});
        QVERIFY(write(source, bytes));
        QVERIFY(write(base + "/Collection/model.civitai.info", "{\"model\":{\"type\":\"Checkpoint\"}}"));
        QVERIFY(write(base + "/Collection/model.preview.png", "preview bytes"));
        QVERIFY(write(base + "/Checkpoint/Collection/model.safetensors", "preserve destination"));
        QVERIFY(write(base + "/legacy-lora.safetensors", weights({"layer.lora_A.weight", "layer.lora_B.weight"})));
        QVERIFY(write(base + "/legacy-pipeline/model_index.json", "{}"));
        QVERIFY(write(base + "/legacy-pipeline/unet/model.safetensors", bytes));
        QVERIFY(write(base + "/mystery.pt", "opaque model"));
        const auto manual = base + "/VAE/keep.safetensors";
        QVERIFY(write(manual, weights({"layer.lora_A.weight"})));
        const auto shared = SharedStorage::open(fixture.path()); QVERIFY(shared);
        QList<QJsonObject> oldReferences;
        for (const auto &model : shared->models())
            if (model.id == "Collection/model.safetensors" || model.id == "legacy-pipeline") oldReferences.append(model.reference(shared->drive().identifier()));
        QCOMPARE(oldReferences.size(), 2);
        const auto store = ModelStore::open(fixture.path()); QVERIFY(store);
        const auto report = store->organize();
        QVERIFY2(report.errors.isEmpty(), qPrintable(report.errors.join('\n')));
        QCOMPARE(report.moved.size(), 4);
        const auto destination = base + "/Checkpoint/Collection/model (1).safetensors";
        QCOMPARE(read(destination), bytes);
        QCOMPARE(read(base + "/Checkpoint/Collection/model.safetensors"), QByteArray("preserve destination"));
        QVERIFY(!QFileInfo::exists(source));
        QCOMPARE(read(base + "/Checkpoint/Collection/model (1).preview.png"), QByteArray("preview bytes"));
        QVERIFY(QFileInfo::exists(base + "/Checkpoint/Collection/model (1).civitai.info"));
        QVERIFY(QFileInfo::exists(base + "/LoRA/legacy-lora.safetensors"));
        QVERIFY(QFileInfo::exists(base + "/Checkpoint/legacy-pipeline/unet/model.safetensors"));
        QVERIFY(QFileInfo::exists(base + "/Other/mystery.pt"));
        QVERIFY(QFileInfo::exists(manual));
        for (const auto &reference : oldReferences) {
            QString error;
            const auto resolved = shared->resolveModel(reference, &error);
            QVERIFY2(!resolved.isEmpty(), qPrintable(error));
            QCOMPARE(resolved, store->resolve(reference.value("path").toString()));
        }
        const auto journal = read(base + "/.model-paths.json");
        const auto repeated = store->organize();
        QVERIFY(repeated.errors.isEmpty()); QVERIFY(repeated.moved.isEmpty());
        QCOMPARE(read(base + "/.model-paths.json"), journal);
        QString error;
        QCOMPARE(store->place(manual, ModelType::LoRA, &error), base + "/LoRA/keep.safetensors");
        QVERIFY(error.isEmpty());
        QCOMPARE(store->resolve("VAE/keep.safetensors"), base + "/LoRA/keep.safetensors");
        QVERIFY(write(destination, "changed weights"));
        for (const auto &reference : oldReferences)
            if (reference.value("path") == "Collection/model.safetensors") QVERIFY(shared->resolveModel(reference, &error).isEmpty());
    }

    void pathReferencesTravelWithTheContainer()
    {
        QTemporaryDir source(SOCIETY_TEST_DIRECTORY "/source-XXXXXX");
        QTemporaryDir replica(SOCIETY_TEST_DIRECTORY "/replica-XXXXXX");
        QVERIFY(source.isValid()); QVERIFY(replica.isValid());
        QVERIFY(SocietyDrive::create(source.path()));
        QVERIFY(SocietyDrive::create(replica.path()));
        const auto bytes = weights({"layer.lora_A.weight"});
        QVERIFY(write(source.filePath("Models/style.safetensors"), bytes));
        const auto store = ModelStore::open(source.path()); QVERIFY(store);
        QCOMPARE(store->organize().moved.size(), 1);
        // A same-identity replica receives the ordinary hidden metadata file with its models.
        QVERIFY(write(replica.filePath(".society-drive.json"), read(source.filePath(".society-drive.json"))));
        QVERIFY(write(replica.filePath("Models/.model-paths.json"), read(source.filePath("Models/.model-paths.json"))));
        QVERIFY(write(replica.filePath("Models/LoRA/style.safetensors"), bytes));
        const auto mirrored = ModelStore::open(replica.path()); QVERIFY(mirrored);
        QCOMPARE(mirrored->resolve("style.safetensors"), replica.filePath("Models/LoRA/style.safetensors"));
        QCOMPARE(mirrored->entries().size(), 1); // The journal is never a selectable model.
        QVERIFY(mirrored->organize().moved.isEmpty());
    }

    void rejectsRedirectedCategoriesInvalidJournalsAndOutsideMoves()
    {
        QTemporaryDir fixture(SOCIETY_TEST_DIRECTORY "/boundaries-XXXXXX"); QVERIFY(fixture.isValid());
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto store = ModelStore::open(fixture.path()); QVERIFY(store);
        const auto path = fixture.filePath("Models/model.safetensors");
        const auto bytes = weights({"layer.lora_A.weight"}); QVERIFY(write(path, bytes));
        QVERIFY(write(fixture.filePath("Files/outside.safetensors"), bytes));
        QString error;
        QVERIFY(store->place("../Files/outside.safetensors", {}, &error).isEmpty());
        QVERIFY(store->place(fixture.filePath("Files/outside.safetensors"), {}, &error).isEmpty());
        QVERIFY(store->place("LoRA", ModelType::Checkpoint, &error).isEmpty());
        QVERIFY(QFile::link(fixture.filePath("Files/outside.safetensors"), fixture.filePath("Models/link.safetensors")));
        QVERIFY(store->place("link.safetensors", {}, &error).isEmpty());
        QVERIFY(QDir().rmdir(fixture.filePath("Models/LoRA")));
        QVERIFY(QFile::link(fixture.filePath("Files"), fixture.filePath("Models/LoRA")));
        QVERIFY(!store->ensureLayout(&error));
        QVERIFY(!store->organize().errors.isEmpty());
        QCOMPARE(read(path), bytes);
        QVERIFY(QFile::remove(fixture.filePath("Models/LoRA")));
        QVERIFY(store->ensureLayout(&error));
        QVERIFY(write(fixture.filePath("Models/.model-paths.json"), "not our journal"));
        QVERIFY(store->place(path, {}, &error).isEmpty());
        QCOMPARE(read(path), bytes);
        QCOMPARE(read(fixture.filePath("Models/.model-paths.json")), QByteArray("not our journal"));
        QVERIFY(QFile::remove(fixture.filePath("Models/.model-paths.json")));
        std::atomic_bool cancelled = true;
        const auto report = store->organize(&cancelled);
        QVERIFY(report.cancelled); QVERIFY(report.moved.isEmpty()); QCOMPARE(read(path), bytes);

        QTemporaryDir conflict(SOCIETY_TEST_DIRECTORY "/type-conflict-XXXXXX"); QVERIFY(conflict.isValid());
        QVERIFY(write(conflict.filePath("Models/Checkpoint"), "do not replace"));
        QVERIFY(!SocietyDrive::create(conflict.path(), &error));
        QCOMPARE(read(conflict.filePath("Models/Checkpoint")), QByteArray("do not replace"));
        QVERIFY(!QFileInfo::exists(conflict.filePath(".society-drive.json")));
    }
};
QTEST_GUILESS_MAIN(ModelStoreTests)
#include "model_store.moc"
