#include "SymbolicLink.h"
#include "ModelStore.h"
#include "SharedStorage.h"
#include <FileCatalog.h>
#include <iiFileProviderDevice.h>
#include <iiFileProviderPresentation.h>

#include <QDataStream>
#include <QCryptographicHash>
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
    void categoryMoveKeepsPortablePresentationWithTheModel() {
        QTemporaryDir fixture(QDir::tempPath() + "/model-presentation-XXXXXX.societycontainer");
        QVERIFY(fixture.isValid()); QVERIFY(SocietyDrive::create(fixture.path()));
        const auto source = fixture.filePath("Models/Checkpoint/model.safetensors");
        const auto target = fixture.filePath("Models/LLM/model.safetensors");
        QVERIFY(write(source, weights({"tensor"})));
        using Presentation = iiFileProvider::native::Presentation;
        const auto native = [](const QString &p) { return std::filesystem::path(p.toStdU16String()); };
        const auto oldPreview = QString::fromStdU16String(Presentation::companionPath(native(source)).u16string());
        const auto newPreview = QString::fromStdU16String(Presentation::companionPath(native(target)).u16string());
        Presentation::cancelPending(native(source));
        const auto original = Presentation::prepare(native(source));
        QVERIFY(!original.document.empty()); QVERIFY(QFileInfo::exists(oldPreview));
        Presentation::enqueue(native(source));
        const auto store = ModelStore::open(fixture.path()); QVERIFY(store);
        QString error; QCOMPARE(store->place(source, ModelType::LLM, &error), target);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!QFileInfo::exists(oldPreview));
        QVERIFY(QFileInfo::exists(newPreview));
        QVERIFY(!read(newPreview).isEmpty());
        QVERIFY(Presentation::waitForIdle(std::chrono::seconds(10)));
        QVERIFY(!QFileInfo::exists(oldPreview));
    }

    void renameKeepsCompanionsAliasesAndManualCategory() {
        QTemporaryDir fixture(QDir::tempPath() + "/model-rename-XXXXXX.societycontainer");
        QVERIFY(fixture.isValid()); QVERIFY(SocietyDrive::create(fixture.path()));
        const auto base = fixture.filePath("Models");
        const auto source = base + "/Other/old.safetensors";
        const auto destination = base + "/Other/new.safetensors";
        const auto oldPreview = base + "/Other/old.preview.png";
        const auto newPreview = base + "/Other/new.preview.png";
        const auto bytes = tensorFile(animaTensors(""));
        QVERIFY(write(source, bytes)); QVERIFY(write(source + ".model.json", "{\"type\":\"Checkpoint\"}"));
        QVERIFY(write(oldPreview, "preview bytes"));
        iiFileProvider::FileCatalog catalog(std::filesystem::path(base.toStdU16String()));
        iiFileProvider::FileMetadata metadata; metadata.title = "Original model";
        QVERIFY(catalog.registerFile("Other/old.safetensors", metadata).metadataWritten);
        const auto portableMetadata = read(source + ".iisacc.json"); QVERIFY(!portableMetadata.isEmpty());
        const auto store = ModelStore::open(fixture.path()); QVERIFY(store);
        QString error;
        QCOMPARE(store->place(source, ModelType::Other, &error), source);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(write(newPreview, "existing preview"));
        QVERIFY(store->rename(source, "new.safetensors", &error).isEmpty());
        QVERIFY(!error.isEmpty()); QCOMPARE(read(source), bytes);
        QCOMPARE(read(newPreview), QByteArray("existing preview"));
        QVERIFY(QFile::remove(newPreview));
        QList<QPair<QString, QString>> moved;
        QCOMPARE(store->rename(source, "new.safetensors", &error, &moved), destination);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(moved.contains(qMakePair(source, destination)));
        QVERIFY(!QFileInfo::exists(source)); QCOMPARE(read(destination), bytes);
        QCOMPARE(read(destination + ".model.json"), QByteArray("{\"type\":\"Checkpoint\"}"));
        QCOMPARE(read(destination + ".iisacc.json"), portableMetadata);
        QCOMPARE(read(newPreview), QByteArray("preview bytes"));
        QVERIFY(!QFileInfo::exists(source + ".iisacc.json"));
        QVERIFY(!QFileInfo::exists(oldPreview));
        QCOMPARE(store->resolve("Other/old.safetensors", &error), destination);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto organized = store->organize();
        QVERIFY2(organized.errors.isEmpty(), qPrintable(organized.errors.join('\n')));
        QVERIFY(QFileInfo::exists(destination)); QVERIFY(organized.moved.isEmpty());
        for (const auto &invalid : {QString(), QStringLiteral(".hidden"), QStringLiteral("../escape"),
                                    QStringLiteral("a/b"), QStringLiteral("reserved.iisacc.json")}) {
            QVERIFY(store->rename(destination, invalid, &error).isEmpty());
            QVERIFY(!error.isEmpty()); QVERIFY(QFileInfo::exists(destination));
        }
    }

    void providerMetadataIsNeverOrganizedAsAModel()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/model-controls-XXXXXX.societycontainer");
        QVERIFY(fixture.isValid());
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto base = fixture.filePath("Models");
        const auto model = base + "/Other/unknown.safetensors";
        const auto companion = model + ".iisacc.json";
        const auto orphan = base + "/Other/orphan.IISACC.JSON";
        const auto bytes = weights({"unknown.weight"});
        QVERIFY(write(model, bytes));
        QVERIFY(write(companion, "{\"type\":\"LoRA\"}"));
        QVERIFY(write(orphan, "{\"type\":\"LoRA\"}"));
        const auto store = ModelStore::open(fixture.path()); QVERIFY(store);
        QString error;
        const auto entries = store->entries(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().relativePath, QString("Other/unknown.safetensors"));
        const auto report = store->organize();
        QVERIFY(report.errors.join('\n').contains("orphan.IISACC.JSON"));
        QVERIFY(report.moved.isEmpty());
        QVERIFY(store->place(orphan, ModelType::LLM, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QCOMPARE(read(model), bytes);
        QCOMPARE(read(companion), QByteArray("{\"type\":\"LoRA\"}"));
        QCOMPARE(read(orphan), QByteArray("{\"type\":\"LoRA\"}"));
        QVERIFY(!QFileInfo::exists(base + "/LoRA/orphan.IISACC.JSON"));
    }

    void reconcilesLegacyMoveWithMetadataStillAtTheOldPath()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/model-legacy-move-XXXXXX.societycontainer");
        QVERIFY(fixture.isValid());
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto source = fixture.filePath("Models/Checkpoint/recover.safetensors");
        const auto destination = fixture.filePath("Models/LLM/recover.safetensors");
        const auto bytes = weights({"tensor"});
        QVERIFY(write(source, bytes));
        const auto storage = SharedStorage::open(fixture.path()); QVERIFY(storage);
        QString error;
        QVERIFY2(storage->recordFile(StoreSection::Models, "Checkpoint/recover.safetensors", &error), qPrintable(error));
        const auto before = storage->fileTracking(StoreSection::Models, "Checkpoint/recover.safetensors");
        QVERIFY(!before.value("objectKey").toString().isEmpty());
        QVERIFY(QDir().mkpath(QFileInfo(destination).absolutePath()));
        QVERIFY(iiFileProvider::FileDevice::rename(source, destination));
        QVERIFY(QFileInfo::exists(source + ".iisacc.json"));
        QVERIFY(!QFileInfo::exists(destination + ".iisacc.json"));
        QVERIFY2(storage->reconcileFiles(&error), qPrintable(error));
        const auto after = storage->fileTracking(StoreSection::Models, "LLM/recover.safetensors", &error);
        QVERIFY2(!after.isEmpty(), qPrintable(error));
        QCOMPARE(after.value("status").toString(), QStringLiteral("ready"));
        QCOMPARE(after.value("objectKey"), before.value("objectKey"));
        QCOMPARE(after.value("resource").toObject().value("contentRevision"), before.value("resource").toObject().value("contentRevision"));
        QCOMPARE(read(destination), bytes);
        QVERIFY(!QFileInfo::exists(source + ".iisacc.json"));
        QVERIFY(QFileInfo::exists(destination + ".iisacc.json"));
    }

    void repairsLegacyCategoryMoveUsingJournalAndNativeIdentity()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/model-repair-alias-XXXXXX.societycontainer");
        QVERIFY(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        const auto source = fixture.filePath("Models/Checkpoint/model.safetensors");
        const auto target = fixture.filePath("Models/LLM/model.safetensors");
        const auto bytes = weights({"tensor"}); QVERIFY(write(source, bytes));
        const auto storage = SharedStorage::open(fixture.path()); QVERIFY(storage);
        QString error;
        QVERIFY2(storage->recordFile(StoreSection::Models, "Checkpoint/model.safetensors", &error), qPrintable(error));
        const auto before = storage->fileTracking(StoreSection::Models, "Checkpoint/model.safetensors");
        const auto metadata = read(source + ".iisacc.json"); QVERIFY(!metadata.isEmpty());
        using Presentation = iiFileProvider::native::Presentation;
        const auto native = [](const QString &p) { return std::filesystem::path(p.toStdU16String()); };
        const auto oldPreview = QString::fromStdU16String(Presentation::companionPath(native(source)).u16string());
        const auto newPreview = QString::fromStdU16String(Presentation::companionPath(native(target)).u16string());
        Presentation::cancelPending(native(source));
        QVERIFY(!Presentation::prepare(native(source)).document.empty());
        const auto preview = read(oldPreview); QVERIFY(!preview.isEmpty());
        QVERIFY(QDir().mkpath(QFileInfo(target).absolutePath()));
        QVERIFY(QFile::rename(source, target)); // Older versions moved only the payload.
        const auto journal = QJsonObject{{"schemaVersion", 1}, {"containerId", drive->identifier()},
            {"aliases", QJsonObject{{"Checkpoint/model.safetensors", "LLM/model.safetensors"}}}};
        QVERIFY(write(fixture.filePath("Models/.model-paths.json"), QJsonDocument(journal).toJson(QJsonDocument::Compact)));
        const auto store = ModelStore::open(fixture.path()); QVERIFY(store);
        QStringList errors;
        Presentation::cancelPending(native(source));
        Presentation::cancelPending(native(target));
        QVERIFY(Presentation::waitForIdle(std::chrono::seconds(10)));
        auto conflict = QJsonDocument::fromJson(preview).object();
        auto conflictPreview = conflict.value("preview").toObject();
        const QByteArray otherPreview("different preview");
        conflictPreview.insert("data", QString::fromLatin1(otherPreview.toBase64()));
        conflictPreview.insert("sha256", QString::fromLatin1(QCryptographicHash::hash(otherPreview, QCryptographicHash::Sha256).toHex()));
        conflict.insert("preview", conflictPreview);
        const auto conflictBytes = QJsonDocument(conflict).toJson(QJsonDocument::Compact);
        QVERIFY(write(newPreview, conflictBytes));
        QCOMPARE(read(newPreview), conflictBytes);
        QVERIFY(store->repairCompanions(&errors).isEmpty()); QVERIFY(!errors.isEmpty());
        QCOMPARE(read(source + ".iisacc.json"), metadata); QCOMPARE(read(oldPreview), preview);
        const auto retained = QJsonDocument::fromJson(read(newPreview)).object().value("preview").toObject();
        QCOMPARE(retained.value("sha256"), conflictPreview.value("sha256"));
        QCOMPARE(retained.value("data"), conflictPreview.value("data"));
        QVERIFY(!QFileInfo::exists(target + ".iisacc.json"));
        QVERIFY(QFile::remove(newPreview)); errors.clear();
        QCOMPARE(store->repairCompanions(&errors), QStringList{target});
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        QVERIFY(!QFileInfo::exists(source + ".iisacc.json"));
        QCOMPARE(read(target + ".iisacc.json"), metadata);
        QVERIFY(!QFileInfo::exists(oldPreview));
        QCOMPARE(QJsonDocument::fromJson(read(newPreview)).object().value("preview"),
                 QJsonDocument::fromJson(preview).object().value("preview"));
        QCOMPARE(read(target), bytes);
        QVERIFY(store->repairCompanions(&errors).isEmpty()); QVERIFY(errors.isEmpty());
        QVERIFY2(storage->reconcileFiles(&error), qPrintable(error));
        const auto after = storage->fileTracking(StoreSection::Models, "LLM/model.safetensors", &error);
        QVERIFY2(!after.isEmpty(), qPrintable(error));
        QCOMPARE(after.value("objectKey"), before.value("objectKey"));
    }

    void legacyRepairRefusesUnverifiedOrConflictingMetadata()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/model-repair-conflict-XXXXXX.societycontainer");
        QVERIFY(fixture.isValid()); const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        const auto source = fixture.filePath("Models/Checkpoint/model.safetensors");
        const auto target = fixture.filePath("Models/LLM/model.safetensors");
        QVERIFY(write(source, weights({"tensor"})));
        const auto storage = SharedStorage::open(fixture.path()); QVERIFY(storage);
        QString error; QVERIFY2(storage->recordFile(StoreSection::Models, "Checkpoint/model.safetensors", &error), qPrintable(error));
        const auto original = read(source + ".iisacc.json"); QVERIFY(!original.isEmpty());
        QVERIFY(QDir().mkpath(QFileInfo(target).absolutePath()));
        QVERIFY(QFile::rename(source, target));
        const auto journal = QJsonObject{{"schemaVersion", 1}, {"containerId", drive->identifier()},
            {"aliases", QJsonObject{{"Checkpoint/model.safetensors", "LLM/model.safetensors"}}}};
        QVERIFY(write(fixture.filePath("Models/.model-paths.json"), QJsonDocument(journal).toJson(QJsonDocument::Compact)));
        QVERIFY(write(target + ".iisacc.json", "foreign metadata"));
        const auto store = ModelStore::open(fixture.path()); QVERIFY(store);
        QStringList errors;
        QVERIFY(store->repairCompanions(&errors).isEmpty()); QVERIFY(!errors.isEmpty());
        QCOMPARE(read(source + ".iisacc.json"), original);
        QCOMPARE(read(target + ".iisacc.json"), QByteArray("foreign metadata"));
        QVERIFY(QFileInfo::exists(target));

        QVERIFY(QFile::remove(target + ".iisacc.json"));
        QVERIFY(QFile::rename(target, target + ".kept"));
        QVERIFY(write(target, weights({"tensor"}))); // Equal content is not equal identity.
        errors.clear(); QVERIFY(store->repairCompanions(&errors).isEmpty());
        QVERIFY(!errors.isEmpty()); QCOMPARE(read(source + ".iisacc.json"), original);
        QVERIFY(!QFileInfo::exists(target + ".iisacc.json"));
    }

    void reportsDetachedClassificationWhenCatalogIsMissing()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/model-repair-no-catalog-XXXXXX.societycontainer");
        QVERIFY(fixture.isValid()); QVERIFY(SocietyDrive::create(fixture.path()));
        const auto sidecar = fixture.filePath("Models/Checkpoint/model.safetensors.iisacc.json");
        const auto document = QJsonDocument(QJsonObject{{"format", "iiFileProvider.Metadata"}, {"schema", 1},
            {"fileKey", "file-unverified"}, {"path", "Checkpoint/model.safetensors"}}).toJson(QJsonDocument::Compact);
        QVERIFY(write(sidecar, document));
        const auto preview = fixture.filePath("Models/Checkpoint/.other.safetensors.iisacc-preview");
        QVERIFY(write(preview, "unverified preview"));
        const auto store = ModelStore::open(fixture.path()); QVERIFY(store); QStringList errors;
        QVERIFY(store->repairCompanions(&errors).isEmpty());
        QVERIFY(errors.join('\n').contains("No model catalog"));
        QVERIFY(errors.join('\n').contains("Detached preview"));
        QCOMPARE(read(sidecar), document);
        QCOMPARE(read(preview), QByteArray("unverified preview"));
        QVERIFY(!QFileInfo::exists(fixture.filePath("Models/.iisacc-file-provider/catalog.sqlite3")));
    }

    void movesPortableMetadataAndPreservesConflictingCompanions()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/model-metadata-move-XXXXXX.societycontainer");
        QVERIFY(fixture.isValid());
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto base = fixture.filePath("Models");
        const auto source = base + "/Checkpoint/style.safetensors";
        const auto preview = base + "/Checkpoint/style.preview.png";
        const auto target = base + "/LLM/style (1).safetensors";
        const auto bytes = weights({"tensor"});
        QVERIFY(write(source, bytes));
        QVERIFY(write(preview, "preview"));
        iiFileProvider::FileMetadata modelMetadata;
        modelMetadata.title = "Model metadata";
        iiFileProvider::FileMetadata previewMetadata;
        previewMetadata.title = "Preview metadata";
        iiFileProvider::FileCatalog catalog(std::filesystem::path(base.toStdU16String()));
        QVERIFY(catalog.registerFile("Checkpoint/style.safetensors", modelMetadata).metadataWritten);
        QVERIFY(catalog.registerFile("Checkpoint/style.preview.png", previewMetadata).metadataWritten);
        const auto modelCompanion = read(source + ".iisacc.json");
        const auto previewCompanion = read(preview + ".iisacc.json");
        QVERIFY(!modelCompanion.isEmpty() && !previewCompanion.isEmpty());
        const auto conflict = base + "/LLM/style.safetensors.iisacc.json";
        QVERIFY(write(conflict, "foreign data"));
        const auto store = ModelStore::open(fixture.path()); QVERIFY(store);
        QString error;
        QCOMPARE(store->place(source, ModelType::LLM, &error), target);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(read(target), bytes);
        QCOMPARE(read(target + ".iisacc.json"), modelCompanion);
        QCOMPARE(read(base + "/LLM/style (1).preview.png.iisacc.json"), previewCompanion);
        QVERIFY(!QFileInfo::exists(source + ".iisacc.json"));
        QVERIFY(!QFileInfo::exists(preview + ".iisacc.json"));
        QCOMPARE(read(conflict), QByteArray("foreign data"));

        const auto invalid = base + "/Checkpoint/invalid.safetensors";
        QVERIFY(write(invalid, bytes));
        QVERIFY(write(invalid + ".iisacc.json", "not provider metadata"));
        QVERIFY(store->place(invalid, ModelType::LLM, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QCOMPARE(read(invalid), bytes);
        QCOMPARE(read(invalid + ".iisacc.json"), QByteArray("not provider metadata"));
    }

    void manualOtherSurvivesOrganizationAndSubsequentMoves()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/manual-model-XXXXXX.societycontainer");
        QVERIFY(fixture.isValid());
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto base = fixture.filePath("Models");
        const auto bytes = weights({"layer.lora_A.weight", "layer.lora_B.weight"});
        QVERIFY(write(base + "/Checkpoint/style.safetensors", bytes));
        QVERIFY(write(base + "/Checkpoint/style.preview.png", "preview"));
        QVERIFY(write(base + "/Other/already.safetensors", bytes));
        QVERIFY(write(base + "/Checkpoint/package/model_index.json", "{}"));
        QVERIFY(write(base + "/Checkpoint/package/unet/weights.safetensors", bytes));
        const auto store = ModelStore::open(fixture.path()); QVERIFY(store);
        QString error;
        QCOMPARE(store->place("Checkpoint/style.safetensors", ModelType::Other, &error), base + "/Other/style.safetensors");
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(store->place("Other/already.safetensors", ModelType::Other, &error), base + "/Other/already.safetensors");
        QCOMPARE(store->place("Checkpoint/package", ModelType::Other, &error), base + "/Other/package");
        // A stale organizer calling the automatic placement path must honor the
        // latest manual decision while holding the model lock.
        QCOMPARE(store->place("Other/style.safetensors", {}, &error), base + "/Other/style.safetensors");
        const auto reopened = ModelStore::open(fixture.path()); QVERIFY(reopened);
        const auto result = reopened->organize();
        QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join('\n')));
        QVERIFY(result.moved.isEmpty());
        QCOMPARE(read(base + "/Other/style.safetensors"), bytes);
        QCOMPARE(read(base + "/Other/style.preview.png"), QByteArray("preview"));
        QCOMPARE(read(base + "/Other/package/unet/weights.safetensors"), bytes);
        QCOMPARE(reopened->resolve("Checkpoint/style.safetensors"), base + "/Other/style.safetensors");
        QCOMPARE(reopened->place("Other/style.safetensors", ModelType::LoRA, &error), base + "/LoRA/style.safetensors");
        QCOMPARE(reopened->resolve("Checkpoint/style.safetensors"), base + "/LoRA/style.safetensors");
        QVERIFY(reopened->organize().moved.isEmpty());
    }

    void exposesBoundedModelMetadataForCatalogs()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/catalog-metadata-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
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
        QVERIFY(createTestSymbolicLink(path, fixture.filePath("linked.safetensors")));
        QVERIFY(ModelClassifier::metadata(fixture.filePath("linked.safetensors")).isEmpty());
    }

    void createsCategoriesOnlyOnDemandAndPreservesLegacyManifest()
    {
        const QStringList expected{"Checkpoint", "Embedding", "Hypernetwork", "Aesthetic Gradient", "LoRA", "LyCORIS", "DoRA",
            "Controlnet", "Upscaler", "Motion", "VAE", "Text Encoder", "UNet", "CLIP Vision", "Poses", "Wildcards",
            "Workflows", "ComfyUI Workflows", "Detection", "VLM", "CLIP", "LLM", "Other"};
        QCOMPARE(allModelTypes().size(), expected.size());
        QTemporaryDir fixture(QDir::tempPath() + "/types-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
        QString error;
        const auto drive = SocietyDrive::create(fixture.path(), &error); QVERIFY2(drive, qPrintable(error));
        const auto store = ModelStore::open(fixture.path(), &error); QVERIFY2(store, qPrintable(error));
        const auto modelsRoot = fixture.filePath("Models");
        const auto manifest = read(fixture.filePath(".society-drive.json"));
        QCOMPARE(QDir(modelsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        QVERIFY(SocietyDrive::create(fixture.path()));
        QVERIFY(store->ensureLayout(&error));
        QVERIFY(store->ensureLayout(&error));
        QCOMPARE(QDir(modelsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        for (qsizetype index = 0; index < expected.size(); ++index) {
            const auto type = allModelTypes()[index];
            QCOMPARE(modelTypeName(type), expected[index]);
            QCOMPARE(modelTypeFromName(expected[index].toLower()), std::optional(type));
            QCOMPARE(store->categoryPath(type), fixture.filePath("Models/" + expected[index]));
            QVERIFY(QFileInfo(store->categoryPath(type)).isDir());
            QVERIFY(QDir().rmdir(store->categoryPath(type))); // Simulate a pre-0.11 Models area.
        }
        QVERIFY(SocietyDrive::open(fixture.path()));
        QVERIFY(QDir(fixture.filePath("Models")).isEmpty()); // Opening remains read-only.
        QVERIFY(store->ensureLayout(&error));
        QVERIFY(store->ensureLayout(&error));
        QCOMPARE(read(fixture.filePath(".society-drive.json")), manifest);
        QCOMPARE(QDir(modelsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        const auto emptyReport = store->organize();
        QVERIFY(emptyReport.errors.isEmpty()); QVERIFY(emptyReport.moved.isEmpty());
        QCOMPARE(QDir(modelsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        QVERIFY(!modelTypeFromName("../Checkpoint"));
    }

    void createsOnlyWrittenCategoriesAndKeepsExistingUserContent()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/lazy-categories-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        const auto identifier = drive->identifier();
        const auto store = ModelStore::open(fixture.path()); QVERIFY(store);
        const auto root = fixture.filePath("Models");
        const auto bytes = weights({"layer.lora_A.weight", "layer.lora_B.weight"});
        QVERIFY(write(root + "/style.safetensors", bytes));
        QVERIFY(write(root + "/style.preview.png", "original preview"));
        QString error;
        QCOMPARE(store->place("style.safetensors", {}, &error), root + "/LoRA/style.safetensors");
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot), QStringList{"LoRA"});
        QCOMPARE(read(root + "/LoRA/style.safetensors"), bytes);
        QCOMPARE(read(root + "/LoRA/style.preview.png"), QByteArray("original preview"));
        QCOMPARE(store->resolve("style.safetensors"), root + "/LoRA/style.safetensors");
        const auto language = gguf("llama"); QVERIFY(write(root + "/language.gguf", language));
        QCOMPARE(store->place("language.gguf", {}, &error), root + "/LLM/language.gguf");
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name), (QStringList{"LLM", "LoRA"}));
        QCOMPARE(read(root + "/LLM/language.gguf"), language);
        const auto checkpoint = store->categoryPath(ModelType::Checkpoint, &error);
        QCOMPARE(checkpoint, root + "/Checkpoint"); QVERIFY(QFileInfo(checkpoint).isDir());
        QVERIFY(write(checkpoint + "/user.safetensors", bytes));
        QVERIFY(QDir().mkdir(root + "/VAE")); // Preserve an existing user-created empty category.
        QVERIFY(store->ensureLayout(&error)); QVERIFY(SocietyDrive::create(fixture.path()));
        QCOMPARE(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name), (QStringList{"Checkpoint", "LLM", "LoRA", "VAE"}));
        QCOMPARE(read(checkpoint + "/user.safetensors"), bytes);
        QCOMPARE(SocietyDrive::open(fixture.path())->identifier(), identifier);
        QCOMPARE(drive->sections().size(), 9);
    }

    void explicitMetadataCoversEveryType()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/metadata-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
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

    void validatesLargeSparseSafetensorsWithoutReadingPayload()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/large-model-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("large.safetensors");
        const qint64 length = (qint64(1) << 32) + 123;
        const QJsonObject header{{"tensor", QJsonObject{{"dtype", "U8"}, {"shape", QJsonArray{length}},
            {"data_offsets", QJsonArray{0, length}}}}};
        const auto prefix = tensorFile(header, {});
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(prefix), prefix.size()); QVERIFY(file.resize(prefix.size() + length)); file.close();
        QVERIFY(ModelClassifier::validateSafetensors(path).isEmpty());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Append));
        QVERIFY(file.resize(prefix.size() + length - 1)); file.close();
        QVERIFY(!ModelClassifier::validateSafetensors(path).isEmpty());
    }
    void kreaCheckpoint_data()
    {
        QTest::addColumn<QString>("prefix");
        QTest::newRow("native") << QString();
        QTest::newRow("comfy") << QString("model.diffusion_model.");
    }
    void kreaCheckpoint()
    {
        QFETCH(QString, prefix);
        QTemporaryDir fixture(QDir::tempPath() + "/krea-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto path = fixture.filePath("Models/Other/unrelated-name.safetensors");
        QJsonObject header{
            {prefix + "first.weight", QJsonObject{{"dtype", "BF16"}, {"shape", QJsonArray{8, 64}}, {"data_offsets", QJsonArray{0, 1024}}}},
            {prefix + "last.linear.weight", QJsonObject{{"dtype", "BF16"}, {"shape", QJsonArray{64, 8}}, {"data_offsets", QJsonArray{1024, 2048}}}},
            {prefix + "txtfusion.projector.weight", QJsonObject{{"dtype", "F32"}, {"shape", QJsonArray{1, 12}}, {"data_offsets", QJsonArray{2048, 2096}}}}
        };
        const auto bytes = tensorFile(header);
        QVERIFY(write(path, bytes));
        QCOMPARE(ModelClassifier::classify(path).type, ModelType::Checkpoint);
        const auto store = ModelStore::open(fixture.path()); QVERIFY(store);
        const auto report = store->organize();
        QVERIFY(report.errors.isEmpty()); QCOMPARE(report.moved.size(), 1);
        QCOMPARE(read(report.moved.first().path), bytes);
        QCOMPARE(store->resolve("Other/unrelated-name.safetensors"), report.moved.first().path);
        QVERIFY(write(path, bytes.chopped(1)));
        QCOMPARE(ModelClassifier::classify(path).type, ModelType::Other);
        auto output = header.value(prefix + "last.linear.weight").toObject();
        output.insert("shape", QJsonArray{32, 16});
        header.insert(prefix + "last.linear.weight", output);
        QVERIFY(write(path, tensorFile(header)));
        QCOMPARE(ModelClassifier::classify(path).type, ModelType::Other);
        QVERIFY(write(path, weights({prefix + "txtfusion.projector.lora_A.weight"})));
        QCOMPARE(ModelClassifier::classify(path).type, ModelType::LoRA);
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
        QTemporaryDir fixture(QDir::tempPath() + "/anima-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
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
        QTemporaryDir fixture(QDir::tempPath() + "/anima-evidence-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
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
        QTemporaryDir fixture(QDir::tempPath() + "/tensor-validation-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
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
        QTemporaryDir fixture(QDir::tempPath() + "/valid-tensors-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
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
        QTemporaryDir fixture(QDir::tempPath() + "/anima-organize-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
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
        QTemporaryDir fixture(QDir::tempPath() + "/tensors-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("unrelated name.safetensors");
        const auto bytes = weights(keys); QVERIFY(write(path, bytes));
        QCOMPARE(int(ModelClassifier::classify(path).type), type);
        QCOMPARE(read(path), bytes);
    }

    void packagesDocumentsAndGguf()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/formats-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
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
        QTemporaryDir fixture(QDir::tempPath() + "/invalid-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
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
        QTemporaryDir fixture(QDir::tempPath() + "/organize-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
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
        QTemporaryDir source(QDir::tempPath() + "/source-XXXXXX.societycontainer");
        QTemporaryDir replica(QDir::tempPath() + "/replica-XXXXXX.societycontainer");
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
        QTemporaryDir fixture(QDir::tempPath() + "/boundaries-XXXXXX.societycontainer"); QVERIFY(fixture.isValid());
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto store = ModelStore::open(fixture.path()); QVERIFY(store);
        const auto path = fixture.filePath("Models/model.safetensors");
        const auto bytes = weights({"layer.lora_A.weight"}); QVERIFY(write(path, bytes));
        QVERIFY(write(fixture.filePath("Files/outside.safetensors"), bytes));
        QString error;
        QVERIFY(store->place("../Files/outside.safetensors", {}, &error).isEmpty());
        QVERIFY(store->place(fixture.filePath("Files/outside.safetensors"), {}, &error).isEmpty());
        QVERIFY(store->place("LoRA", ModelType::Checkpoint, &error).isEmpty());
        QVERIFY(createTestSymbolicLink(fixture.filePath("Files/outside.safetensors"), fixture.filePath("Models/link.safetensors")));
        QVERIFY(store->place("link.safetensors", {}, &error).isEmpty());
        QVERIFY(!QFileInfo::exists(fixture.filePath("Models/LoRA")));
        QVERIFY(createTestSymbolicLink(fixture.filePath("Files"), fixture.filePath("Models/LoRA")));
        QVERIFY(!store->ensureLayout(&error));
        QVERIFY(!store->organize().errors.isEmpty());
        QCOMPARE(read(path), bytes);
        QVERIFY(removeTestSymbolicLink(fixture.filePath("Models/LoRA")));
        QVERIFY(store->ensureLayout(&error));
        QVERIFY(write(fixture.filePath("Models/.model-paths.json"), "not our journal"));
        QVERIFY(store->place(path, {}, &error).isEmpty());
        QCOMPARE(read(path), bytes);
        QCOMPARE(read(fixture.filePath("Models/.model-paths.json")), QByteArray("not our journal"));
        QVERIFY(QFile::remove(fixture.filePath("Models/.model-paths.json")));
        std::atomic_bool cancelled = true;
        const auto report = store->organize(&cancelled);
        QVERIFY(report.cancelled); QVERIFY(report.moved.isEmpty()); QCOMPARE(read(path), bytes);

        QTemporaryDir conflict(QDir::tempPath() + "/type-conflict-XXXXXX.societycontainer"); QVERIFY(conflict.isValid());
        QVERIFY(write(conflict.filePath("Models/Checkpoint"), "do not replace"));
        QVERIFY(!SocietyDrive::create(conflict.path(), &error));
        QCOMPARE(read(conflict.filePath("Models/Checkpoint")), QByteArray("do not replace"));
        QVERIFY(!QFileInfo::exists(conflict.filePath(".society-drive.json")));
    }
};
QTEST_GUILESS_MAIN(ModelStoreTests)
#include "model_store.moc"
