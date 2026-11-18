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
#include "CTestSupport.h"
#include <QCoreApplication>
#include <QEvent>
#include <string>
#include <vector>
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
public:
    void categoryMoveKeepsPortablePresentationWithTheModel() {
        QTemporaryDir fixture(QDir::tempPath() + "/model-presentation-XXXXXX.societycontainer");
        CTEST_CHECK(fixture.isValid()); CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto source = fixture.filePath("Models/Checkpoint/model.safetensors");
        const auto target = fixture.filePath("Models/LLM/model.safetensors");
        CTEST_CHECK(write(source, weights({"tensor"})));
        using Presentation = iiFileProvider::native::Presentation;
        const auto native = [](const QString &p) { return std::filesystem::path(p.toStdU16String()); };
        const auto oldPreview = QString::fromStdU16String(Presentation::companionPath(native(source)).u16string());
        const auto newPreview = QString::fromStdU16String(Presentation::companionPath(native(target)).u16string());
        Presentation::cancelPending(native(source));
        const auto original = Presentation::prepare(native(source));
        CTEST_CHECK(!original.document.empty()); CTEST_CHECK(QFileInfo::exists(oldPreview));
        Presentation::enqueue(native(source));
        const auto store = ModelStore::open(fixture.path()); CTEST_CHECK(store);
        QString error; CTEST_COMPARE(store->place(source, ModelType::LLM, &error), target);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        CTEST_CHECK(!QFileInfo::exists(oldPreview));
        CTEST_CHECK(QFileInfo::exists(newPreview));
        CTEST_CHECK(!read(newPreview).isEmpty());
        CTEST_CHECK(Presentation::waitForIdle(std::chrono::seconds(10)));
        CTEST_CHECK(!QFileInfo::exists(oldPreview));
    }

    void renameKeepsCompanionsAliasesAndManualCategory() {
        QTemporaryDir fixture(QDir::tempPath() + "/model-rename-XXXXXX.societycontainer");
        CTEST_CHECK(fixture.isValid()); CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto base = fixture.filePath("Models");
        const auto source = base + "/Other/old.safetensors";
        const auto destination = base + "/Other/new.safetensors";
        const auto oldPreview = base + "/Other/old.preview.png";
        const auto newPreview = base + "/Other/new.preview.png";
        const auto bytes = tensorFile(animaTensors(""));
        CTEST_CHECK(write(source, bytes)); CTEST_CHECK(write(source + ".model.json", "{\"type\":\"Checkpoint\"}"));
        CTEST_CHECK(write(oldPreview, "preview bytes"));
        iiFileProvider::FileCatalog catalog(std::filesystem::path(base.toStdU16String()));
        iiFileProvider::FileMetadata metadata; metadata.title = "Original model";
        CTEST_CHECK(catalog.registerFile("Other/old.safetensors", metadata).metadataWritten);
        const auto portableMetadata = read(source + ".iisacc.json"); CTEST_CHECK(!portableMetadata.isEmpty());
        const auto store = ModelStore::open(fixture.path()); CTEST_CHECK(store);
        QString error;
        CTEST_COMPARE(store->place(source, ModelType::Other, &error), source);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        CTEST_CHECK(write(newPreview, "existing preview"));
        CTEST_CHECK(store->rename(source, "new.safetensors", &error).isEmpty());
        CTEST_CHECK(!error.isEmpty()); CTEST_COMPARE(read(source), bytes);
        CTEST_COMPARE(read(newPreview), QByteArray("existing preview"));
        CTEST_CHECK(QFile::remove(newPreview));
        QList<QPair<QString, QString>> moved;
        CTEST_COMPARE(store->rename(source, "new.safetensors", &error, &moved), destination);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        CTEST_CHECK(moved.contains(qMakePair(source, destination)));
        CTEST_CHECK(!QFileInfo::exists(source)); CTEST_COMPARE(read(destination), bytes);
        CTEST_COMPARE(read(destination + ".model.json"), QByteArray("{\"type\":\"Checkpoint\"}"));
        CTEST_COMPARE(read(fixture.filePath(".society-runtime/classifications/models/Other/new.safetensors.iisacc.json")), portableMetadata);
        CTEST_CHECK(!QFileInfo::exists(destination + ".iisacc.json"));
        CTEST_COMPARE(read(newPreview), QByteArray("preview bytes"));
        CTEST_CHECK(!QFileInfo::exists(source + ".iisacc.json"));
        CTEST_CHECK(!QFileInfo::exists(oldPreview));
        CTEST_COMPARE(store->resolve("Other/old.safetensors", &error), destination);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        const auto organized = store->organize();
        CTEST_CHECK_MESSAGE(organized.errors.isEmpty(), qPrintable(organized.errors.join('\n')));
        CTEST_CHECK(QFileInfo::exists(destination)); CTEST_CHECK(organized.moved.isEmpty());
        for (const auto &invalid : {QString(), QStringLiteral(".hidden"), QStringLiteral("../escape"),
                                    QStringLiteral("a/b"), QStringLiteral("reserved.iisacc.json")}) {
            CTEST_CHECK(store->rename(destination, invalid, &error).isEmpty());
            CTEST_CHECK(!error.isEmpty()); CTEST_CHECK(QFileInfo::exists(destination));
        }
    }

    void providerMetadataIsNeverOrganizedAsAModel()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/model-controls-XXXXXX.societycontainer");
        CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto base = fixture.filePath("Models");
        const auto model = base + "/Other/unknown.safetensors";
        const auto companion = model + ".iisacc.json";
        const auto orphan = base + "/Other/orphan.IISACC.JSON";
        const auto bytes = weights({"unknown.weight"});
        CTEST_CHECK(write(model, bytes));
        CTEST_CHECK(write(companion, "{\"type\":\"LoRA\"}"));
        CTEST_CHECK(write(orphan, "{\"type\":\"LoRA\"}"));
        const auto store = ModelStore::open(fixture.path()); CTEST_CHECK(store);
        QString error;
        const auto entries = store->entries(&error);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        CTEST_COMPARE(entries.size(), 1);
        CTEST_COMPARE(entries.first().relativePath, QString("Other/unknown.safetensors"));
        const auto report = store->organize();
        CTEST_CHECK(report.errors.join('\n').contains("orphan.IISACC.JSON"));
        CTEST_CHECK(report.moved.isEmpty());
        CTEST_CHECK(store->place(orphan, ModelType::LLM, &error).isEmpty());
        CTEST_CHECK(!error.isEmpty());
        CTEST_COMPARE(read(model), bytes);
        CTEST_COMPARE(read(companion), QByteArray("{\"type\":\"LoRA\"}"));
        CTEST_COMPARE(read(orphan), QByteArray("{\"type\":\"LoRA\"}"));
        CTEST_CHECK(!QFileInfo::exists(base + "/LoRA/orphan.IISACC.JSON"));
    }

    void reconcilesLegacyMoveWithMetadataStillAtTheOldPath()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/model-legacy-move-XXXXXX.societycontainer");
        CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto source = fixture.filePath("Models/Checkpoint/recover.safetensors");
        const auto destination = fixture.filePath("Models/LLM/recover.safetensors");
        const auto bytes = weights({"tensor"});
        CTEST_CHECK(write(source, bytes));
        const auto storage = SharedStorage::open(fixture.path()); CTEST_CHECK(storage);
        QString error;
        CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::Models, "Checkpoint/recover.safetensors", &error), qPrintable(error));
        const auto before = storage->fileTracking(StoreSection::Models, "Checkpoint/recover.safetensors");
        CTEST_CHECK(!before.value("objectKey").toString().isEmpty());
        CTEST_CHECK(QFile::rename(fixture.filePath(".society-runtime/classifications/models/Checkpoint/recover.safetensors.iisacc.json"),
                              source + ".iisacc.json"));
        CTEST_CHECK(QDir().mkpath(QFileInfo(destination).absolutePath()));
        CTEST_CHECK(iiFileProvider::FileDevice::rename(source, destination));
        CTEST_CHECK(QFileInfo::exists(source + ".iisacc.json"));
        CTEST_CHECK(!QFileInfo::exists(destination + ".iisacc.json"));
        CTEST_CHECK_MESSAGE(storage->reconcileFiles(&error), qPrintable(error));
        const auto after = storage->fileTracking(StoreSection::Models, "LLM/recover.safetensors", &error);
        CTEST_CHECK_MESSAGE(!after.isEmpty(), qPrintable(error));
        CTEST_COMPARE(after.value("status").toString(), QStringLiteral("ready"));
        CTEST_COMPARE(after.value("objectKey"), before.value("objectKey"));
        CTEST_COMPARE(after.value("resource").toObject().value("contentRevision"), before.value("resource").toObject().value("contentRevision"));
        CTEST_COMPARE(read(destination), bytes);
        CTEST_CHECK(!QFileInfo::exists(source + ".iisacc.json"));
        CTEST_CHECK(QFileInfo::exists(fixture.filePath(".society-runtime/classifications/models/LLM/recover.safetensors.iisacc.json")));
    }

    void repairsLegacyCategoryMoveUsingJournalAndNativeIdentity()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/model-repair-alias-XXXXXX.societycontainer");
        CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        const auto source = fixture.filePath("Models/Checkpoint/model.safetensors");
        const auto target = fixture.filePath("Models/LLM/model.safetensors");
        const auto bytes = weights({"tensor"}); CTEST_CHECK(write(source, bytes));
        const auto storage = SharedStorage::open(fixture.path()); CTEST_CHECK(storage);
        QString error;
        CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::Models, "Checkpoint/model.safetensors", &error), qPrintable(error));
        const auto before = storage->fileTracking(StoreSection::Models, "Checkpoint/model.safetensors");
        CTEST_CHECK(QFile::rename(fixture.filePath(".society-runtime/classifications/models/Checkpoint/model.safetensors.iisacc.json"),
                              source + ".iisacc.json"));
        const auto metadata = read(source + ".iisacc.json"); CTEST_CHECK(!metadata.isEmpty());
        using Presentation = iiFileProvider::native::Presentation;
        const auto native = [](const QString &p) { return std::filesystem::path(p.toStdU16String()); };
        const auto oldPreview = QString::fromStdU16String(Presentation::companionPath(native(source)).u16string());
        const auto newPreview = QString::fromStdU16String(Presentation::companionPath(native(target)).u16string());
        Presentation::cancelPending(native(source));
        CTEST_CHECK(!Presentation::prepare(native(source)).document.empty());
        const auto preview = read(oldPreview); CTEST_CHECK(!preview.isEmpty());
        CTEST_CHECK(QDir().mkpath(QFileInfo(target).absolutePath()));
        CTEST_CHECK(QFile::rename(source, target)); // Older versions moved only the payload.
        const auto journal = QJsonObject{{"schemaVersion", 1}, {"containerId", drive->identifier()},
            {"aliases", QJsonObject{{"Checkpoint/model.safetensors", "LLM/model.safetensors"}}}};
        CTEST_CHECK(write(fixture.filePath("Models/.model-paths.json"), QJsonDocument(journal).toJson(QJsonDocument::Compact)));
        const auto store = ModelStore::open(fixture.path()); CTEST_CHECK(store);
        QStringList errors;
        Presentation::cancelPending(native(source));
        Presentation::cancelPending(native(target));
        CTEST_CHECK(Presentation::waitForIdle(std::chrono::seconds(10)));
        auto conflict = QJsonDocument::fromJson(preview).object();
        auto conflictPreview = conflict.value("preview").toObject();
        const QByteArray otherPreview("different preview");
        conflictPreview.insert("data", QString::fromLatin1(otherPreview.toBase64()));
        conflictPreview.insert("sha256", QString::fromLatin1(QCryptographicHash::hash(otherPreview, QCryptographicHash::Sha256).toHex()));
        conflict.insert("preview", conflictPreview);
        const auto conflictBytes = QJsonDocument(conflict).toJson(QJsonDocument::Compact);
        CTEST_CHECK(write(newPreview, conflictBytes));
        CTEST_COMPARE(read(newPreview), conflictBytes);
        CTEST_CHECK(store->repairCompanions(&errors).isEmpty()); CTEST_CHECK(!errors.isEmpty());
        CTEST_COMPARE(read(source + ".iisacc.json"), metadata); CTEST_COMPARE(read(oldPreview), preview);
        const auto retained = QJsonDocument::fromJson(read(newPreview)).object().value("preview").toObject();
        CTEST_COMPARE(retained.value("sha256"), conflictPreview.value("sha256"));
        CTEST_COMPARE(retained.value("data"), conflictPreview.value("data"));
        CTEST_CHECK(!QFileInfo::exists(target + ".iisacc.json"));
        CTEST_CHECK(QFile::remove(newPreview)); errors.clear();
        CTEST_COMPARE(store->repairCompanions(&errors), QStringList{target});
        CTEST_CHECK_MESSAGE(errors.isEmpty(), qPrintable(errors.join('\n')));
        CTEST_CHECK(!QFileInfo::exists(source + ".iisacc.json"));
        const auto recovered = read(fixture.filePath(".society-runtime/classifications/models/LLM/model.safetensors.iisacc.json"));
        CTEST_CHECK(!recovered.isEmpty());
        CTEST_COMPARE(QJsonDocument::fromJson(recovered).object().value("fileKey"),
                 QJsonDocument::fromJson(metadata).object().value("fileKey"));
        CTEST_CHECK(!QFileInfo::exists(oldPreview));
        CTEST_COMPARE(QJsonDocument::fromJson(read(newPreview)).object().value("preview"),
                 QJsonDocument::fromJson(preview).object().value("preview"));
        CTEST_COMPARE(read(target), bytes);
        CTEST_CHECK(store->repairCompanions(&errors).isEmpty()); CTEST_CHECK(errors.isEmpty());
        CTEST_CHECK_MESSAGE(storage->reconcileFiles(&error), qPrintable(error));
        const auto after = storage->fileTracking(StoreSection::Models, "LLM/model.safetensors", &error);
        CTEST_CHECK_MESSAGE(!after.isEmpty(), qPrintable(error));
        CTEST_COMPARE(after.value("objectKey"), before.value("objectKey"));
    }

    void legacyRepairRefusesUnverifiedOrConflictingMetadata()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/model-repair-conflict-XXXXXX.societycontainer");
        CTEST_CHECK(fixture.isValid()); const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        const auto source = fixture.filePath("Models/Checkpoint/model.safetensors");
        const auto target = fixture.filePath("Models/LLM/model.safetensors");
        CTEST_CHECK(write(source, weights({"tensor"})));
        const auto storage = SharedStorage::open(fixture.path()); CTEST_CHECK(storage);
        QString error; CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::Models, "Checkpoint/model.safetensors", &error), qPrintable(error));
        CTEST_CHECK(QFile::rename(fixture.filePath(".society-runtime/classifications/models/Checkpoint/model.safetensors.iisacc.json"),
                              source + ".iisacc.json"));
        const auto original = read(source + ".iisacc.json"); CTEST_CHECK(!original.isEmpty());
        CTEST_CHECK(QDir().mkpath(QFileInfo(target).absolutePath()));
        CTEST_CHECK(QFile::rename(source, target));
        const auto journal = QJsonObject{{"schemaVersion", 1}, {"containerId", drive->identifier()},
            {"aliases", QJsonObject{{"Checkpoint/model.safetensors", "LLM/model.safetensors"}}}};
        CTEST_CHECK(write(fixture.filePath("Models/.model-paths.json"), QJsonDocument(journal).toJson(QJsonDocument::Compact)));
        CTEST_CHECK(write(target + ".iisacc.json", "foreign metadata"));
        const auto store = ModelStore::open(fixture.path()); CTEST_CHECK(store);
        QStringList errors;
        CTEST_CHECK(store->repairCompanions(&errors).isEmpty()); CTEST_CHECK(!errors.isEmpty());
        CTEST_COMPARE(read(source + ".iisacc.json"), original);
        CTEST_COMPARE(read(target + ".iisacc.json"), QByteArray("foreign metadata"));
        CTEST_CHECK(QFileInfo::exists(target));

        CTEST_CHECK(QFile::remove(target + ".iisacc.json"));
        CTEST_CHECK(QFile::rename(target, target + ".kept"));
        CTEST_CHECK(write(target, weights({"tensor"}))); // Equal content is not equal identity.
        errors.clear(); CTEST_CHECK(store->repairCompanions(&errors).isEmpty());
        CTEST_CHECK(!errors.isEmpty()); CTEST_COMPARE(read(source + ".iisacc.json"), original);
        CTEST_CHECK(!QFileInfo::exists(target + ".iisacc.json"));
    }

    void reportsDetachedClassificationWhenCatalogIsMissing()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/model-repair-no-catalog-XXXXXX.societycontainer");
        CTEST_CHECK(fixture.isValid()); CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto sidecar = fixture.filePath("Models/Checkpoint/model.safetensors.iisacc.json");
        const auto document = QJsonDocument(QJsonObject{{"format", "iiFileProvider.Metadata"}, {"schema", 1},
            {"fileKey", "file-unverified"}, {"path", "Checkpoint/model.safetensors"}}).toJson(QJsonDocument::Compact);
        CTEST_CHECK(write(sidecar, document));
        const auto preview = fixture.filePath("Models/Checkpoint/.other.safetensors.iisacc-preview");
        CTEST_CHECK(write(preview, "unverified preview"));
        const auto store = ModelStore::open(fixture.path()); CTEST_CHECK(store); QStringList errors;
        CTEST_CHECK(store->repairCompanions(&errors).isEmpty());
        CTEST_CHECK(errors.join('\n').contains("No model catalog"));
        CTEST_CHECK(errors.join('\n').contains("Detached preview"));
        CTEST_COMPARE(read(sidecar), document);
        CTEST_COMPARE(read(preview), QByteArray("unverified preview"));
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("Models/.iisacc-file-provider/catalog.sqlite3")));
    }

    void movesPortableMetadataAndPreservesConflictingCompanions()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/model-metadata-move-XXXXXX.societycontainer");
        CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto base = fixture.filePath("Models");
        const auto source = base + "/Checkpoint/style.safetensors";
        const auto preview = base + "/Checkpoint/style.preview.png";
        const auto target = base + "/LLM/style (1).safetensors";
        const auto bytes = weights({"tensor"});
        CTEST_CHECK(write(source, bytes));
        CTEST_CHECK(write(preview, "preview"));
        iiFileProvider::FileMetadata modelMetadata;
        modelMetadata.title = "Model metadata";
        iiFileProvider::FileMetadata previewMetadata;
        previewMetadata.title = "Preview metadata";
        iiFileProvider::FileCatalog catalog(std::filesystem::path(base.toStdU16String()));
        CTEST_CHECK(catalog.registerFile("Checkpoint/style.safetensors", modelMetadata).metadataWritten);
        CTEST_CHECK(catalog.registerFile("Checkpoint/style.preview.png", previewMetadata).metadataWritten);
        const auto modelCompanion = read(source + ".iisacc.json");
        const auto previewCompanion = read(preview + ".iisacc.json");
        CTEST_CHECK(!modelCompanion.isEmpty() && !previewCompanion.isEmpty());
        const auto conflict = base + "/LLM/style.safetensors.iisacc.json";
        CTEST_CHECK(write(conflict, "foreign data"));
        const auto store = ModelStore::open(fixture.path()); CTEST_CHECK(store);
        QString error;
        CTEST_COMPARE(store->place(source, ModelType::LLM, &error), target);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        CTEST_COMPARE(read(target), bytes);
        CTEST_COMPARE(read(fixture.filePath(".society-runtime/classifications/models/LLM/style (1).safetensors.iisacc.json")), modelCompanion);
        CTEST_COMPARE(read(fixture.filePath(".society-runtime/classifications/models/LLM/style (1).preview.png.iisacc.json")), previewCompanion);
        CTEST_CHECK(!QFileInfo::exists(source + ".iisacc.json"));
        CTEST_CHECK(!QFileInfo::exists(preview + ".iisacc.json"));
        CTEST_COMPARE(read(conflict), QByteArray("foreign data"));

        const auto invalid = base + "/Checkpoint/invalid.safetensors";
        CTEST_CHECK(write(invalid, bytes));
        CTEST_CHECK(write(invalid + ".iisacc.json", "not provider metadata"));
        CTEST_CHECK(store->place(invalid, ModelType::LLM, &error).isEmpty());
        CTEST_CHECK(!error.isEmpty());
        CTEST_COMPARE(read(invalid), bytes);
        CTEST_COMPARE(read(invalid + ".iisacc.json"), QByteArray("not provider metadata"));
    }

    void manualOtherSurvivesOrganizationAndSubsequentMoves()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/manual-model-XXXXXX.societycontainer");
        CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto base = fixture.filePath("Models");
        const auto bytes = weights({"layer.lora_A.weight", "layer.lora_B.weight"});
        CTEST_CHECK(write(base + "/Checkpoint/style.safetensors", bytes));
        CTEST_CHECK(write(base + "/Checkpoint/style.preview.png", "preview"));
        CTEST_CHECK(write(base + "/Other/already.safetensors", bytes));
        CTEST_CHECK(write(base + "/Checkpoint/package/model_index.json", "{}"));
        CTEST_CHECK(write(base + "/Checkpoint/package/unet/weights.safetensors", bytes));
        const auto store = ModelStore::open(fixture.path()); CTEST_CHECK(store);
        QString error;
        CTEST_COMPARE(store->place("Checkpoint/style.safetensors", ModelType::Other, &error), base + "/Other/style.safetensors");
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        CTEST_COMPARE(store->place("Other/already.safetensors", ModelType::Other, &error), base + "/Other/already.safetensors");
        CTEST_COMPARE(store->place("Checkpoint/package", ModelType::Other, &error), base + "/Other/package");
        // A stale organizer calling the automatic placement path must honor the
        // latest manual decision while holding the model lock.
        CTEST_COMPARE(store->place("Other/style.safetensors", {}, &error), base + "/Other/style.safetensors");
        const auto reopened = ModelStore::open(fixture.path()); CTEST_CHECK(reopened);
        const auto result = reopened->organize();
        CTEST_CHECK_MESSAGE(result.errors.isEmpty(), qPrintable(result.errors.join('\n')));
        CTEST_CHECK(result.moved.isEmpty());
        CTEST_COMPARE(read(base + "/Other/style.safetensors"), bytes);
        CTEST_COMPARE(read(base + "/Other/style.preview.png"), QByteArray("preview"));
        CTEST_COMPARE(read(base + "/Other/package/unet/weights.safetensors"), bytes);
        CTEST_COMPARE(reopened->resolve("Checkpoint/style.safetensors"), base + "/Other/style.safetensors");
        CTEST_COMPARE(reopened->place("Other/style.safetensors", ModelType::LoRA, &error), base + "/LoRA/style.safetensors");
        CTEST_COMPARE(reopened->resolve("Checkpoint/style.safetensors"), base + "/LoRA/style.safetensors");
        CTEST_CHECK(reopened->organize().moved.isEmpty());
    }

    void exposesBoundedModelMetadataForCatalogs()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/catalog-metadata-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        const auto path = fixture.filePath("model.safetensors");
        CTEST_CHECK(write(path, weights({"tensor"}, {{"modelspec.architecture", "stable-diffusion-xl-v1-base"}})));
        auto metadata = ModelClassifier::metadata(path);
        CTEST_COMPARE(metadata.value("modelspec.architecture").toString(), QString("stable-diffusion-xl-v1-base"));
        CTEST_COMPARE(metadata.value("tensor_dtypes").toArray(), QJsonArray{"F32"});
        CTEST_CHECK(write(path + ".model.json", "{\"society.modality\":\"video\"}"));
        CTEST_COMPARE(ModelClassifier::metadata(path).value("society.modality").toString(), QString("video"));
        CTEST_CHECK(write(fixture.filePath("voice/config.json"), "{\"architectures\":[\"WhisperForConditionalGeneration\"],\"torch_dtype\":\"float16\"}"));
        CTEST_COMPARE(ModelClassifier::metadata(fixture.filePath("voice")).value("torch_dtype").toString(), QString("float16"));
        CTEST_CHECK(write(fixture.filePath("language.gguf"), gguf("llama")));
        CTEST_COMPARE(ModelClassifier::metadata(fixture.filePath("language.gguf")).value("general.architecture").toString(), QString("llama"));
        CTEST_CHECK(write(fixture.filePath("invalid.safetensors"), QByteArray::fromHex("ffffffffffffffff")));
        CTEST_CHECK(ModelClassifier::metadata(fixture.filePath("invalid.safetensors")).isEmpty());
        CTEST_CHECK(createTestSymbolicLink(path, fixture.filePath("linked.safetensors")));
        CTEST_CHECK(ModelClassifier::metadata(fixture.filePath("linked.safetensors")).isEmpty());
    }

    void createsAndRepairsEveryCategoryWithoutChangingTheManifest()
    {
        const QStringList expected{"Checkpoint", "Embedding", "Hypernetwork", "Aesthetic Gradient", "LoRA", "LyCORIS", "DoRA",
            "Controlnet", "Upscaler", "Motion", "VAE", "Text Encoder", "UNet", "CLIP Vision", "Poses", "Wildcards",
            "Workflows", "ComfyUI Workflows", "Detection", "VLM", "CLIP", "LLM", "Other"};
        CTEST_COMPARE(allModelTypes().size(), expected.size());
        QTemporaryDir fixture(QDir::tempPath() + "/types-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        QString error;
        const auto drive = SocietyDrive::create(fixture.path(), &error); CTEST_CHECK_MESSAGE(drive, qPrintable(error));
        const auto store = ModelStore::open(fixture.path(), &error); CTEST_CHECK_MESSAGE(store, qPrintable(error));
        const auto modelsRoot = fixture.filePath("Models");
        const auto manifest = read(fixture.filePath(".society-drive.json"));
        CTEST_COMPARE(QDir(modelsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), expected.size());
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        CTEST_CHECK(store->ensureLayout(&error));
        CTEST_CHECK(store->ensureLayout(&error));
        for (qsizetype index = 0; index < expected.size(); ++index) {
            const auto type = allModelTypes()[index];
            CTEST_COMPARE(modelTypeName(type), expected[index]);
            CTEST_COMPARE(modelTypeFromName(expected[index].toLower()), std::optional(type));
            CTEST_COMPARE(store->categoryPath(type), fixture.filePath("Models/" + expected[index]));
            CTEST_CHECK(QFileInfo(store->categoryPath(type)).isDir());
            CTEST_CHECK(QDir().rmdir(store->categoryPath(type))); // Simulate an older partial layout.
        }
        CTEST_CHECK(SocietyDrive::open(fixture.path()));
        CTEST_CHECK(QDir(fixture.filePath("Models")).isEmpty()); // Opening remains read-only.
        std::stop_source cancelled; cancelled.request_stop();
        CTEST_CHECK(!store->ensureLayout(&error, cancelled.get_token()));
        CTEST_CHECK(QDir(modelsRoot).isEmpty());
        CTEST_CHECK(store->ensureLayout(&error));
        CTEST_CHECK(store->ensureLayout(&error));
        CTEST_COMPARE(read(fixture.filePath(".society-drive.json")), manifest);
        CTEST_COMPARE(QDir(modelsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), expected.size());
        for (const auto &name : expected) CTEST_CHECK(QFileInfo(modelsRoot + '/' + name).isDir());
        const auto emptyReport = store->organize();
        CTEST_CHECK(emptyReport.errors.isEmpty()); CTEST_CHECK(emptyReport.moved.isEmpty());
        CTEST_COMPARE(QDir(modelsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), expected.size());
        CTEST_CHECK(!modelTypeFromName("../Checkpoint"));
    }

    void keepsAllCategoriesAndExistingUserContent()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/lazy-categories-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        const auto identifier = drive->identifier();
        const auto store = ModelStore::open(fixture.path()); CTEST_CHECK(store);
        const auto root = fixture.filePath("Models");
        const auto bytes = weights({"layer.lora_A.weight", "layer.lora_B.weight"});
        CTEST_CHECK(write(root + "/style.safetensors", bytes));
        CTEST_CHECK(write(root + "/style.preview.png", "original preview"));
        QString error;
        CTEST_COMPARE(store->place("style.safetensors", {}, &error), root + "/LoRA/style.safetensors");
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        CTEST_COMPARE(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), allModelTypes().size());
        CTEST_COMPARE(read(root + "/LoRA/style.safetensors"), bytes);
        CTEST_COMPARE(read(root + "/LoRA/style.preview.png"), QByteArray("original preview"));
        CTEST_COMPARE(store->resolve("style.safetensors"), root + "/LoRA/style.safetensors");
        const auto language = gguf("llama"); CTEST_CHECK(write(root + "/language.gguf", language));
        CTEST_COMPARE(store->place("language.gguf", {}, &error), root + "/LLM/language.gguf");
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        CTEST_COMPARE(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), allModelTypes().size());
        CTEST_COMPARE(read(root + "/LLM/language.gguf"), language);
        const auto checkpoint = store->categoryPath(ModelType::Checkpoint, &error);
        CTEST_COMPARE(checkpoint, root + "/Checkpoint"); CTEST_CHECK(QFileInfo(checkpoint).isDir());
        CTEST_CHECK(write(checkpoint + "/user.safetensors", bytes));
        CTEST_CHECK(QFileInfo(root + "/VAE").isDir());
        CTEST_CHECK(QDir().rmdir(root + "/VAE"));
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        CTEST_CHECK(QFileInfo(root + "/VAE").isDir());
        CTEST_CHECK(store->ensureLayout(&error));
        CTEST_COMPARE(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), allModelTypes().size());
        CTEST_COMPARE(read(checkpoint + "/user.safetensors"), bytes);
        CTEST_COMPARE(SocietyDrive::open(fixture.path())->identifier(), identifier);
        CTEST_COMPARE(drive->sections().size(), 9);
    }

    void layoutConflictPreservesTheExistingEntryAndOtherMissingCategories()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/layout-conflict-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        const auto store = ModelStore::open(fixture.path()); CTEST_CHECK(store);
        const auto checkpoint = fixture.filePath("Models/Checkpoint");
        const auto other = fixture.filePath("Models/LoRA");
        CTEST_CHECK(QDir().rmdir(checkpoint)); CTEST_CHECK(QDir().rmdir(other));
        CTEST_CHECK(write(checkpoint, "preserve this file"));
        QString error;
        CTEST_CHECK(!store->ensureLayout(&error)); CTEST_CHECK(error.contains("conflict", Qt::CaseInsensitive));
        CTEST_COMPARE(read(checkpoint), QByteArray("preserve this file"));
        CTEST_CHECK(!QFileInfo::exists(other));
        CTEST_COMPARE(SocietyDrive::open(fixture.path())->identifier(), drive->identifier());
    }

    void explicitMetadataCoversEveryType()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/metadata-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        const auto path = fixture.filePath("arbitrary.safetensors");
        for (const auto type : allModelTypes()) {
            CTEST_CHECK(write(path, weights({"tensor"}, {{"society.model_type", modelTypeName(type)}})));
            const auto found = ModelClassifier::classify(path);
            CTEST_COMPARE(found.type, type); CTEST_CHECK(found.recognized); CTEST_CHECK(!found.evidence.isEmpty());
        }
        const auto legacy = fixture.filePath("arbitrary.pt");
        CTEST_CHECK(write(legacy, "legacy data"));
        CTEST_CHECK(write(fixture.filePath("arbitrary.civitai.info"), "{\"model\":{\"type\":\"Hypernetwork\"}}"));
        CTEST_COMPARE(ModelClassifier::classify(legacy).type, ModelType::Hypernetwork);
        CTEST_COMPARE(ModelClassifier::companionFiles(legacy), QStringList{fixture.filePath("arbitrary.civitai.info")});
    }

    void validatesLargeSparseSafetensorsWithoutReadingPayload()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/large-model-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        const auto path = fixture.filePath("large.safetensors");
        const qint64 length = (qint64(1) << 32) + 123;
        const QJsonObject header{{"tensor", QJsonObject{{"dtype", "U8"}, {"shape", QJsonArray{length}},
            {"data_offsets", QJsonArray{0, length}}}}};
        const auto prefix = tensorFile(header, {});
        QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly));
        CTEST_COMPARE(file.write(prefix), prefix.size()); CTEST_CHECK(file.resize(prefix.size() + length)); file.close();
        CTEST_CHECK(ModelClassifier::validateSafetensors(path).isEmpty());
        CTEST_CHECK(file.open(QIODevice::WriteOnly | QIODevice::Append));
        CTEST_CHECK(file.resize(prefix.size() + length - 1)); file.close();
        CTEST_CHECK(!ModelClassifier::validateSafetensors(path).isEmpty());
    }
    void kreaCheckpoint(const QString &prefix)
    {
        QTemporaryDir fixture(QDir::tempPath() + "/krea-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto path = fixture.filePath("Models/Other/unrelated-name.safetensors");
        QJsonObject header{
            {prefix + "first.weight", QJsonObject{{"dtype", "BF16"}, {"shape", QJsonArray{8, 64}}, {"data_offsets", QJsonArray{0, 1024}}}},
            {prefix + "last.linear.weight", QJsonObject{{"dtype", "BF16"}, {"shape", QJsonArray{64, 8}}, {"data_offsets", QJsonArray{1024, 2048}}}},
            {prefix + "txtfusion.projector.weight", QJsonObject{{"dtype", "F32"}, {"shape", QJsonArray{1, 12}}, {"data_offsets", QJsonArray{2048, 2096}}}}
        };
        const auto bytes = tensorFile(header);
        CTEST_CHECK(write(path, bytes));
        CTEST_COMPARE(ModelClassifier::classify(path).type, ModelType::Checkpoint);
        const auto store = ModelStore::open(fixture.path()); CTEST_CHECK(store);
        const auto report = store->organize();
        CTEST_CHECK(report.errors.isEmpty()); CTEST_COMPARE(report.moved.size(), 1);
        CTEST_COMPARE(read(report.moved.first().path), bytes);
        CTEST_COMPARE(store->resolve("Other/unrelated-name.safetensors"), report.moved.first().path);
        CTEST_CHECK(write(path, bytes.chopped(1)));
        CTEST_COMPARE(ModelClassifier::classify(path).type, ModelType::Other);
        auto output = header.value(prefix + "last.linear.weight").toObject();
        output.insert("shape", QJsonArray{32, 16});
        header.insert(prefix + "last.linear.weight", output);
        CTEST_CHECK(write(path, tensorFile(header)));
        CTEST_COMPARE(ModelClassifier::classify(path).type, ModelType::Other);
        CTEST_CHECK(write(path, weights({prefix + "txtfusion.projector.lora_A.weight"})));
        CTEST_COMPARE(ModelClassifier::classify(path).type, ModelType::LoRA);
    }

    void animaCheckpoint(const QString &prefix)
    {
        QTemporaryDir fixture(QDir::tempPath() + "/anima-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
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
        const auto bytes = tensorFile(header); CTEST_CHECK(write(path, bytes));
        const auto found = ModelClassifier::classify(path);
        CTEST_COMPARE(found.type, ModelType::Checkpoint); CTEST_CHECK(found.recognized);
        CTEST_CHECK(found.evidence.contains("Anima"));
        CTEST_COMPARE(read(path), bytes);
    }

    void animaRequiresConsistentBackboneAndAdapter()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/anima-evidence-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        const auto path = fixture.filePath("anima_checkpoint.safetensors");
        CTEST_CHECK(write(path, weights({"net.blocks.0.mlp.layer1.weight", "net.llm_adapter.blocks.0.cross_attn.q_proj.weight"})));
        CTEST_COMPARE(ModelClassifier::classify(path).type, ModelType::Other);
        auto header = animaTensors("net.");
        header.insert("unrelated.final_layer.linear.weight", header.take("net.final_layer.linear.weight"));
        CTEST_CHECK(write(path, tensorFile(header)));
        CTEST_COMPARE(ModelClassifier::classify(path).type, ModelType::Other);
        header = animaTensors("net.");
        auto projection = header.value("net.x_embedder.proj.1.weight").toObject();
        projection.insert("shape", QJsonArray{16, 34}); // Same byte count, incompatible hidden dimension.
        header.insert("net.x_embedder.proj.1.weight", projection);
        CTEST_CHECK(write(path, tensorFile(header)));
        CTEST_COMPARE(ModelClassifier::classify(path).type, ModelType::Other);
        CTEST_CHECK(write(path, weights({"net.blocks.0.mlp.layer1.lora_A.weight", "net.blocks.0.mlp.layer1.lora_B.weight"},
            {{"modelspec.architecture", "anima"}})));
        CTEST_COMPARE(ModelClassifier::classify(path).type, ModelType::LoRA);
    }

    static std::vector<std::pair<std::string, QByteArray>> tensorValidationRows()
    {
        std::vector<std::pair<std::string, QByteArray>> rows;
        const QJsonObject tensor{{"dtype", "F32"}, {"shape", QJsonArray{1}}, {"data_offsets", QJsonArray{0, 4}}};
        const auto row = [&](const char *name, const char *field, const QJsonValue &value) {
            auto invalid = tensor; invalid.insert(QLatin1String(field), value);
            rows.emplace_back(name, tensorFile(QJsonObject{{"layer.lora_A.weight", invalid}}, QByteArray(4, '\0')));
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
        rows.emplace_back("invalid-descriptor", tensorFile({{"layer.lora_A.weight", "not a tensor"}}, QByteArray(4, '\0')));
        rows.emplace_back("truncated-data", tensorFile({{"layer.lora_A.weight", tensor}}, QByteArray(3, '\0')));
        rows.emplace_back("unindexed-data", tensorFile({{"layer.lora_A.weight", tensor}}, QByteArray(5, '\0')));
        rows.emplace_back("overlapping-data", tensorFile({{"layer.lora_A.weight", tensor}, {"layer.lora_B.weight", tensor}}, QByteArray(4, '\0')));
        auto later = tensor; later.insert("data_offsets", QJsonArray{8, 12});
        rows.emplace_back("gap-in-data", tensorFile({{"layer.lora_A.weight", tensor}, {"layer.lora_B.weight", later}}, QByteArray(12, '\0')));
        rows.emplace_back("non-string-metadata", tensorFile({{"layer.lora_A.weight", tensor}, {"__metadata__", QJsonObject{{"count", 1}}}}, QByteArray(4, '\0')));
        rows.emplace_back("invalid-metadata", tensorFile({{"layer.lora_A.weight", tensor}, {"__metadata__", "LoRA"}}, QByteArray(4, '\0')));
        rows.emplace_back("metadata-without-tensors", tensorFile({{"__metadata__", QJsonObject{{"society.model_type", "Checkpoint"}}}}, {}));
        const auto descriptor = QJsonDocument(tensor).toJson(QJsonDocument::Compact);
        const QList<QPair<const char *, QByteArray>> duplicates{
            {"duplicate-tensor", QByteArray("{\"layer.lora_A.weight\":") + descriptor + ",\"layer.lora_A.weight\":" + descriptor + '}'},
            {"duplicate-field", "{\"layer.lora_A.weight\":{\"dtype\":\"F32\",\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]}}"},
            {"escaped-duplicate-field", "{\"layer.lora_A.weight\":{\"dtype\":\"F32\",\"dtyp\\u0065\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]}}"}};
        for (const auto &[name, json] : duplicates) {
            QByteArray bytes(8, '\0'); qToLittleEndian(quint64(json.size()), bytes.data());
            rows.emplace_back(name, bytes + json + QByteArray(4, '\0'));
        }
        return rows;
    }
    void validatesTensorDescriptorsAndPayload(const QByteArray &bytes)
    {
        QTemporaryDir fixture(QDir::tempPath() + "/tensor-validation-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        const auto path = fixture.filePath("checkpoint.safetensors"); CTEST_CHECK(write(path, bytes));
        const auto result = ModelClassifier::classify(path);
        CTEST_CHECK_MESSAGE(!result.recognized, qPrintable(result.evidence));
        CTEST_COMPARE(result.type, ModelType::Other); CTEST_CHECK(!result.evidence.isEmpty());
        CTEST_CHECK(ModelClassifier::metadata(path).isEmpty());
        CTEST_CHECK(write(path + ".model.json", "{\"type\":\"Checkpoint\"}"));
        CTEST_CHECK(!ModelClassifier::classify(path).recognized); // Sidecars cannot legitimize broken tensor files.
        CTEST_COMPARE(read(path), bytes);
    }

    void acceptsScalarEmptyAndPackedTensors()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/valid-tensors-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        const auto path = fixture.filePath("model.safetensors");
        const QJsonObject header{
            {"layer.lora_A.weight", QJsonObject{{"dtype", "F32"}, {"shape", QJsonArray{}}, {"data_offsets", QJsonArray{0, 4}}}},
            {"empty", QJsonObject{{"dtype", "BF16"}, {"shape", QJsonArray{0, 8}}, {"data_offsets", QJsonArray{4, 4}}}},
            {"packed", QJsonObject{{"dtype", "F4"}, {"shape", QJsonArray{2}}, {"data_offsets", QJsonArray{4, 5}}}}};
        CTEST_CHECK(write(path, tensorFile(header, QByteArray(5, '\0'))));
        CTEST_COMPARE(ModelClassifier::classify(path).type, ModelType::LoRA);
        CTEST_COMPARE(ModelClassifier::metadata(path).value("tensor_dtypes").toArray(), (QJsonArray{"BF16", "F32", "F4"}));
        const auto temporary = fixture.filePath("provider.tmp");
        CTEST_CHECK(QFile::rename(path, temporary));
        CTEST_COMPARE(ModelClassifier::classify(temporary, "original.SAFETENSORS").type, ModelType::LoRA);
    }

    void reorganizesPreviouslyUnknownAnimaAndPreservesReferences()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/anima-organize-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto source = fixture.filePath("Models/Other/renamed.safetensors");
        const auto bytes = tensorFile(animaTensors("net.")); CTEST_CHECK(write(source, bytes));
        CTEST_CHECK(write(source + ".preview.png", "preview"));
        const auto shared = SharedStorage::open(fixture.path()); CTEST_CHECK(shared);
        const auto reference = shared->models().first().reference(shared->drive().identifier());
        const auto store = ModelStore::open(fixture.path()); CTEST_CHECK(store);
        const auto report = store->organize();
        CTEST_CHECK_MESSAGE(report.errors.isEmpty(), qPrintable(report.errors.join('\n'))); CTEST_COMPARE(report.moved.size(), 1);
        const auto destination = fixture.filePath("Models/Checkpoint/renamed.safetensors");
        CTEST_COMPARE(report.moved.first().path, destination); CTEST_COMPARE(read(destination), bytes);
        CTEST_COMPARE(read(destination + ".preview.png"), QByteArray("preview"));
        CTEST_COMPARE(store->resolve("Other/renamed.safetensors"), destination);
        CTEST_COMPARE(shared->resolveModel(reference), destination);
        CTEST_CHECK(store->organize().moved.isEmpty());
    }

    struct SignatureRow { std::string name; QStringList keys; int type; };
    static std::vector<SignatureRow> tensorSignatureRows()
    {
        std::vector<SignatureRow> rows;
        const auto row = [&](const char *name, const QStringList &keys, ModelType type) { rows.push_back({name, keys, int(type)}); };
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
        return rows;
    }
    void tensorSignatures(const QStringList &keys, int type)
    {
        QTemporaryDir fixture(QDir::tempPath() + "/tensors-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        const auto path = fixture.filePath("unrelated name.safetensors");
        const auto bytes = weights(keys); CTEST_CHECK(write(path, bytes));
        CTEST_COMPARE(int(ModelClassifier::classify(path).type), type);
        CTEST_COMPARE(read(path), bytes);
    }

    void packagesDocumentsAndGguf()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/formats-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        const auto pipeline = fixture.filePath("pipeline");
        CTEST_CHECK(write(pipeline + "/model_index.json", "{\"_class_name\":\"StableDiffusionPipeline\"}"));
        CTEST_CHECK(ModelClassifier::isPackage(pipeline));
        CTEST_COMPARE(ModelClassifier::classify(pipeline).type, ModelType::Checkpoint);
        CTEST_CHECK(write(fixture.filePath("adapter/adapter_config.json"), "{\"peft_type\":\"LORA\",\"use_dora\":true}"));
        CTEST_COMPARE(ModelClassifier::classify(fixture.filePath("adapter")).type, ModelType::DoRA);
        const auto config = fixture.filePath("component/config.json");
        CTEST_CHECK(write(fixture.filePath("component/model.safetensors"), weights({"tensor"})));
        for (const auto &[name, type] : QList<QPair<QString, ModelType>>{{"ControlNetModel", ModelType::ControlNet},
                {"AutoencoderKL", ModelType::VAE}, {"CLIPVisionModelWithProjection", ModelType::CLIPVision},
                {"T5EncoderModel", ModelType::TextEncoder}, {"CLIPModel", ModelType::CLIP},
                {"LlamaForCausalLM", ModelType::LLM}, {"LlavaForConditionalGeneration", ModelType::VLM},
                {"RTDetrForObjectDetection", ModelType::Detection}, {"RRDBNet", ModelType::Upscaler},
                {"MotionAdapter", ModelType::Motion}, {"UNet2DConditionModel", ModelType::UNet}}) {
            CTEST_CHECK(write(config, QJsonDocument(QJsonObject{{"architectures", QJsonArray{name}}}).toJson()));
            CTEST_CHECK(ModelClassifier::isPackage(fixture.filePath("component")));
            CTEST_COMPARE(ModelClassifier::classify(fixture.filePath("component")).type, type);
        }
        for (const auto &[json, type] : QList<QPair<QByteArray, ModelType>>{
                {"{\"nodes\":[],\"links\":[]}", ModelType::ComfyUIWorkflows},
                {"{\"1\":{\"class_type\":\"KSampler\",\"inputs\":{}}}", ModelType::ComfyUIWorkflows},
                {"{\"people\":[{\"pose_keypoints_2d\":[1,2,3]}]}", ModelType::Poses},
                {"{\"workflow\":{\"steps\":[]}}", ModelType::Workflows}}) {
            const auto path = fixture.filePath("asset.json"); CTEST_CHECK(write(path, json));
            CTEST_COMPARE(ModelClassifier::classify(path).type, type);
        }
        CTEST_CHECK(write(fixture.filePath("colors.txt"), "red\nblue\ngreen\n"));
        CTEST_COMPARE(ModelClassifier::classify(fixture.filePath("colors.txt")).type, ModelType::Wildcards);
        const auto path = fixture.filePath("model.gguf");
        for (const auto &[name, type] : QList<QPair<QString, ModelType>>{{"llama", ModelType::LLM}, {"qwen2vl", ModelType::VLM},
                {"flux", ModelType::UNet}, {"t5encoder", ModelType::TextEncoder}, {"unknown-architecture", ModelType::Other}}) {
            CTEST_CHECK(write(path, gguf(name))); CTEST_COMPARE(ModelClassifier::classify(path).type, type);
        }
        CTEST_CHECK(write(path, gguf("clip", false, true))); CTEST_COMPARE(ModelClassifier::classify(path).type, ModelType::CLIPVision);
        CTEST_CHECK(write(path, gguf("clip", true, true))); CTEST_COMPARE(ModelClassifier::classify(path).type, ModelType::CLIP);
    }

    void boundedInspectionDoesNotGuessFromNamesOrExecuteLegacyFiles()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/invalid-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        QByteArray oversized(8, '\0'); qToLittleEndian(quint64(256 * 1024 * 1024), oversized.data());
        for (const auto &bytes : {QByteArray(), QByteArray("not a tensor file"), oversized,
                QByteArray::fromHex("1000000000000000") + QByteArray("{}")} ) {
            const auto path = fixture.filePath("checkpoint_lora_vae.safetensors"); CTEST_CHECK(write(path, bytes));
            const auto result = ModelClassifier::classify(path);
            CTEST_CHECK(!result.recognized); CTEST_COMPARE(result.type, ModelType::Other); CTEST_COMPARE(read(path), bytes);
        }
        const auto legacy = fixture.filePath("Llama.pt"); CTEST_CHECK(write(legacy, "__reduce__ execute_me"));
        CTEST_COMPARE(ModelClassifier::classify(legacy).type, ModelType::Other);
        CTEST_CHECK(write(fixture.filePath("corrupt.gguf"), "GGUF"));
        CTEST_COMPARE(ModelClassifier::classify(fixture.filePath("corrupt.gguf")).type, ModelType::Other);
    }

    void organizesPackagesAndCompanionsWithoutReplacingFilesOrReferences()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/organize-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto base = fixture.filePath("Models");
        const auto source = base + "/Collection/model.safetensors";
        const auto bytes = weights({"model.diffusion_model.input_blocks.0.weight", "first_stage_model.encoder.weight"});
        CTEST_CHECK(write(source, bytes));
        CTEST_CHECK(write(base + "/Collection/model.civitai.info", "{\"model\":{\"type\":\"Checkpoint\"}}"));
        CTEST_CHECK(write(base + "/Collection/model.preview.png", "preview bytes"));
        CTEST_CHECK(write(base + "/Checkpoint/Collection/model.safetensors", "preserve destination"));
        CTEST_CHECK(write(base + "/legacy-lora.safetensors", weights({"layer.lora_A.weight", "layer.lora_B.weight"})));
        CTEST_CHECK(write(base + "/legacy-pipeline/model_index.json", "{}"));
        CTEST_CHECK(write(base + "/legacy-pipeline/unet/model.safetensors", bytes));
        CTEST_CHECK(write(base + "/mystery.pt", "opaque model"));
        const auto manual = base + "/VAE/keep.safetensors";
        CTEST_CHECK(write(manual, weights({"layer.lora_A.weight"})));
        const auto shared = SharedStorage::open(fixture.path()); CTEST_CHECK(shared);
        QList<QJsonObject> oldReferences;
        for (const auto &model : shared->models())
            if (model.id == "Collection/model.safetensors" || model.id == "legacy-pipeline") oldReferences.append(model.reference(shared->drive().identifier()));
        CTEST_COMPARE(oldReferences.size(), 2);
        const auto store = ModelStore::open(fixture.path()); CTEST_CHECK(store);
        const auto report = store->organize();
        CTEST_CHECK_MESSAGE(report.errors.isEmpty(), qPrintable(report.errors.join('\n')));
        CTEST_COMPARE(report.moved.size(), 4);
        const auto destination = base + "/Checkpoint/Collection/model (1).safetensors";
        CTEST_COMPARE(read(destination), bytes);
        CTEST_COMPARE(read(base + "/Checkpoint/Collection/model.safetensors"), QByteArray("preserve destination"));
        CTEST_CHECK(!QFileInfo::exists(source));
        CTEST_COMPARE(read(base + "/Checkpoint/Collection/model (1).preview.png"), QByteArray("preview bytes"));
        CTEST_CHECK(QFileInfo::exists(base + "/Checkpoint/Collection/model (1).civitai.info"));
        CTEST_CHECK(QFileInfo::exists(base + "/LoRA/legacy-lora.safetensors"));
        CTEST_CHECK(QFileInfo::exists(base + "/Checkpoint/legacy-pipeline/unet/model.safetensors"));
        CTEST_CHECK(QFileInfo::exists(base + "/Other/mystery.pt"));
        CTEST_CHECK(QFileInfo::exists(manual));
        for (const auto &reference : oldReferences) {
            QString error;
            const auto resolved = shared->resolveModel(reference, &error);
            CTEST_CHECK_MESSAGE(!resolved.isEmpty(), qPrintable(error));
            CTEST_COMPARE(resolved, store->resolve(reference.value("path").toString()));
        }
        const auto journal = read(base + "/.model-paths.json");
        const auto repeated = store->organize();
        CTEST_CHECK(repeated.errors.isEmpty()); CTEST_CHECK(repeated.moved.isEmpty());
        CTEST_COMPARE(read(base + "/.model-paths.json"), journal);
        QString error;
        CTEST_COMPARE(store->place(manual, ModelType::LoRA, &error), base + "/LoRA/keep.safetensors");
        CTEST_CHECK(error.isEmpty());
        CTEST_COMPARE(store->resolve("VAE/keep.safetensors"), base + "/LoRA/keep.safetensors");
        CTEST_CHECK(write(destination, "changed weights"));
        for (const auto &reference : oldReferences)
            if (reference.value("path") == "Collection/model.safetensors") CTEST_CHECK(shared->resolveModel(reference, &error).isEmpty());
    }

    void pathReferencesTravelWithTheContainer()
    {
        QTemporaryDir source(QDir::tempPath() + "/source-XXXXXX.societycontainer");
        QTemporaryDir replica(QDir::tempPath() + "/replica-XXXXXX.societycontainer");
        CTEST_CHECK(source.isValid()); CTEST_CHECK(replica.isValid());
        CTEST_CHECK(SocietyDrive::create(source.path()));
        CTEST_CHECK(SocietyDrive::create(replica.path()));
        const auto bytes = weights({"layer.lora_A.weight"});
        CTEST_CHECK(write(source.filePath("Models/style.safetensors"), bytes));
        const auto store = ModelStore::open(source.path()); CTEST_CHECK(store);
        CTEST_COMPARE(store->organize().moved.size(), 1);
        // A same-identity replica receives the ordinary hidden metadata file with its models.
        CTEST_CHECK(write(replica.filePath(".society-drive.json"), read(source.filePath(".society-drive.json"))));
        CTEST_CHECK(write(replica.filePath("Models/.model-paths.json"), read(source.filePath("Models/.model-paths.json"))));
        CTEST_CHECK(write(replica.filePath("Models/LoRA/style.safetensors"), bytes));
        const auto mirrored = ModelStore::open(replica.path()); CTEST_CHECK(mirrored);
        CTEST_COMPARE(mirrored->resolve("style.safetensors"), replica.filePath("Models/LoRA/style.safetensors"));
        CTEST_COMPARE(mirrored->entries().size(), 1); // The journal is never a selectable model.
        CTEST_CHECK(mirrored->organize().moved.isEmpty());
    }

    void rejectsRedirectedCategoriesInvalidJournalsAndOutsideMoves()
    {
        QTemporaryDir fixture(QDir::tempPath() + "/boundaries-XXXXXX.societycontainer"); CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto store = ModelStore::open(fixture.path()); CTEST_CHECK(store);
        const auto path = fixture.filePath("Models/model.safetensors");
        const auto bytes = weights({"layer.lora_A.weight"}); CTEST_CHECK(write(path, bytes));
        CTEST_CHECK(write(fixture.filePath("Files/outside.safetensors"), bytes));
        QString error;
        CTEST_CHECK(store->place("../Files/outside.safetensors", {}, &error).isEmpty());
        CTEST_CHECK(store->place(fixture.filePath("Files/outside.safetensors"), {}, &error).isEmpty());
        CTEST_CHECK(store->place("LoRA", ModelType::Checkpoint, &error).isEmpty());
        CTEST_CHECK(createTestSymbolicLink(fixture.filePath("Files/outside.safetensors"), fixture.filePath("Models/link.safetensors")));
        CTEST_CHECK(store->place("link.safetensors", {}, &error).isEmpty());
        CTEST_CHECK(QDir().rmdir(fixture.filePath("Models/LoRA")));
        CTEST_CHECK(createTestSymbolicLink(fixture.filePath("Files"), fixture.filePath("Models/LoRA")));
        CTEST_CHECK(!store->ensureLayout(&error));
        CTEST_CHECK(!store->organize().errors.isEmpty());
        CTEST_COMPARE(read(path), bytes);
        CTEST_CHECK(removeTestSymbolicLink(fixture.filePath("Models/LoRA")));
        CTEST_CHECK(store->ensureLayout(&error));
        CTEST_CHECK(write(fixture.filePath("Models/.model-paths.json"), "not our journal"));
        CTEST_CHECK(store->place(path, {}, &error).isEmpty());
        CTEST_COMPARE(read(path), bytes);
        CTEST_COMPARE(read(fixture.filePath("Models/.model-paths.json")), QByteArray("not our journal"));
        CTEST_CHECK(QFile::remove(fixture.filePath("Models/.model-paths.json")));
        std::atomic_bool cancelled = true;
        const auto report = store->organize(&cancelled);
        CTEST_CHECK(report.cancelled); CTEST_CHECK(report.moved.isEmpty()); CTEST_COMPARE(read(path), bytes);

        QTemporaryDir conflict(QDir::tempPath() + "/type-conflict-XXXXXX.societycontainer"); CTEST_CHECK(conflict.isValid());
        CTEST_CHECK(write(conflict.filePath("Models/Checkpoint"), "do not replace"));
        CTEST_CHECK(!SocietyDrive::create(conflict.path(), &error));
        CTEST_COMPARE(read(conflict.filePath("Models/Checkpoint")), QByteArray("do not replace"));
        CTEST_CHECK(!QFileInfo::exists(conflict.filePath(".society-drive.json")));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    society_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    ModelStoreTests tests;
    struct FlushDeferredDeletes {
        ~FlushDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
    };
    runner.run("categoryMoveKeepsPortablePresentationWithTheModel", [&] { FlushDeferredDeletes cleanup; tests.categoryMoveKeepsPortablePresentationWithTheModel(); });
    runner.run("renameKeepsCompanionsAliasesAndManualCategory", [&] { FlushDeferredDeletes cleanup; tests.renameKeepsCompanionsAliasesAndManualCategory(); });
    runner.run("providerMetadataIsNeverOrganizedAsAModel", [&] { FlushDeferredDeletes cleanup; tests.providerMetadataIsNeverOrganizedAsAModel(); });
    runner.run("reconcilesLegacyMoveWithMetadataStillAtTheOldPath", [&] { FlushDeferredDeletes cleanup; tests.reconcilesLegacyMoveWithMetadataStillAtTheOldPath(); });
    runner.run("repairsLegacyCategoryMoveUsingJournalAndNativeIdentity", [&] { FlushDeferredDeletes cleanup; tests.repairsLegacyCategoryMoveUsingJournalAndNativeIdentity(); });
    runner.run("legacyRepairRefusesUnverifiedOrConflictingMetadata", [&] { FlushDeferredDeletes cleanup; tests.legacyRepairRefusesUnverifiedOrConflictingMetadata(); });
    runner.run("reportsDetachedClassificationWhenCatalogIsMissing", [&] { FlushDeferredDeletes cleanup; tests.reportsDetachedClassificationWhenCatalogIsMissing(); });
    runner.run("movesPortableMetadataAndPreservesConflictingCompanions", [&] { FlushDeferredDeletes cleanup; tests.movesPortableMetadataAndPreservesConflictingCompanions(); });
    runner.run("manualOtherSurvivesOrganizationAndSubsequentMoves", [&] { FlushDeferredDeletes cleanup; tests.manualOtherSurvivesOrganizationAndSubsequentMoves(); });
    runner.run("exposesBoundedModelMetadataForCatalogs", [&] { FlushDeferredDeletes cleanup; tests.exposesBoundedModelMetadataForCatalogs(); });
    runner.run("createsAndRepairsEveryCategoryWithoutChangingTheManifest", [&] { FlushDeferredDeletes cleanup; tests.createsAndRepairsEveryCategoryWithoutChangingTheManifest(); });
    runner.run("keepsAllCategoriesAndExistingUserContent", [&] { FlushDeferredDeletes cleanup; tests.keepsAllCategoriesAndExistingUserContent(); });
    runner.run("layoutConflictPreservesTheExistingEntryAndOtherMissingCategories", [&] { FlushDeferredDeletes cleanup; tests.layoutConflictPreservesTheExistingEntryAndOtherMissingCategories(); });
    runner.run("explicitMetadataCoversEveryType", [&] { FlushDeferredDeletes cleanup; tests.explicitMetadataCoversEveryType(); });
    runner.run("validatesLargeSparseSafetensorsWithoutReadingPayload", [&] { FlushDeferredDeletes cleanup; tests.validatesLargeSparseSafetensorsWithoutReadingPayload(); });
    runner.run("kreaCheckpoint:native", [&] { FlushDeferredDeletes cleanup; tests.kreaCheckpoint(QStringLiteral("")); });
    runner.run("kreaCheckpoint:comfy", [&] { FlushDeferredDeletes cleanup; tests.kreaCheckpoint(QStringLiteral("model.diffusion_model.")); });
    runner.run("animaCheckpoint:unwrapped", [&] { FlushDeferredDeletes cleanup; tests.animaCheckpoint(QStringLiteral("")); });
    runner.run("animaCheckpoint:base-net", [&] { FlushDeferredDeletes cleanup; tests.animaCheckpoint(QStringLiteral("net.")); });
    runner.run("animaCheckpoint:finetune", [&] { FlushDeferredDeletes cleanup; tests.animaCheckpoint(QStringLiteral("model.diffusion_model.")); });
    runner.run("animaCheckpoint:complete", [&] { FlushDeferredDeletes cleanup; tests.animaCheckpoint(QStringLiteral("model.diffusion_model.net.")); });
    runner.run("animaRequiresConsistentBackboneAndAdapter", [&] { FlushDeferredDeletes cleanup; tests.animaRequiresConsistentBackboneAndAdapter(); });
    for (const auto &row : ModelStoreTests::tensorValidationRows())
        runner.run("validatesTensorDescriptorsAndPayload:" + row.first, [&] { FlushDeferredDeletes cleanup; tests.validatesTensorDescriptorsAndPayload(row.second); });
    runner.run("acceptsScalarEmptyAndPackedTensors", [&] { FlushDeferredDeletes cleanup; tests.acceptsScalarEmptyAndPackedTensors(); });
    runner.run("reorganizesPreviouslyUnknownAnimaAndPreservesReferences", [&] { FlushDeferredDeletes cleanup; tests.reorganizesPreviouslyUnknownAnimaAndPreservesReferences(); });
    for (const auto &row : ModelStoreTests::tensorSignatureRows())
        runner.run("tensorSignatures:" + row.name, [&] { FlushDeferredDeletes cleanup; tests.tensorSignatures(row.keys, row.type); });
    runner.run("packagesDocumentsAndGguf", [&] { FlushDeferredDeletes cleanup; tests.packagesDocumentsAndGguf(); });
    runner.run("boundedInspectionDoesNotGuessFromNamesOrExecuteLegacyFiles", [&] { FlushDeferredDeletes cleanup; tests.boundedInspectionDoesNotGuessFromNamesOrExecuteLegacyFiles(); });
    runner.run("organizesPackagesAndCompanionsWithoutReplacingFilesOrReferences", [&] { FlushDeferredDeletes cleanup; tests.organizesPackagesAndCompanionsWithoutReplacingFilesOrReferences(); });
    runner.run("pathReferencesTravelWithTheContainer", [&] { FlushDeferredDeletes cleanup; tests.pathReferencesTravelWithTheContainer(); });
    runner.run("rejectsRedirectedCategoriesInvalidJournalsAndOutsideMoves", [&] { FlushDeferredDeletes cleanup; tests.rejectsRedirectedCategoriesInvalidJournalsAndOutsideMoves(); });
    return runner.result();
}
