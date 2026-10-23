#include "SymbolicLink.h"
#include <iiFileProviderPresentation.h>
#include <iiFileProviderNative.h>
#include <File.h>
#include <FileCatalog.h>
#include <SharedStorage.h>
#include <StorageMap.h>
#include <StorageDirectoryModel.h>
#include <StorageModelCatalog.h>
#include <StorageMaintenance.h>
#include <ModelStore.h>
#include <FileOperations.h>
#include <QSignalSpy>
#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <algorithm>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QElapsedTimer>
#include <QDataStream>
#include <QScopeGuard>

using namespace iiSocietyContainer;

namespace {
void write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}
void makeReadyReplica(const SocietyDrive &drive)
{
    // Keep this fixture's logical namespace stable while creating the actual
    // replica manifest contract, rather than treating missing primary metadata
    // as proof that a newly created source package is a replica.
    QVERIFY(SocietyDrive::adoptReplicaIdentity(drive.rootPath(), drive.identifier(), drive.identifier()));
    QVERIFY(SocietyDrive::completeReplica(drive.rootPath(), drive.identifier()));
}
}

class SharedStorageTests : public QObject
{
    Q_OBJECT
private slots:
    void fileInformationExposesProviderPresentationAndClassification() {
        QTemporaryDir fixture(QDir::current().filePath("rich-information-XXXXXX.societycontainer")); QVERIFY(fixture.isValid());
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto storage = SharedStorage::open(fixture.path()); QVERIFY(storage);
        const auto root = std::filesystem::path(fixture.filePath("Files").toStdU16String());
        iiFileProvider::FileCatalog catalog(root);
        iiFileProvider::FileMetadata annotation; annotation.title = "Campaign document"; annotation.description = "Approved copy";
        annotation.keywords = {"campaign", "approved"}; annotation.rating = 5; annotation.color = iiFileProvider::ColorLabel::Blue;
        annotation.fields = {{"project", "launch"}};
        annotation.collections = {catalog.createCollection("Launch assets").key};
        const std::string body = "Original searchable document";
        const auto created = catalog.createFile("Brief #한글.txt", {reinterpret_cast<const std::byte *>(body.data()), body.size()}, annotation);
        QVERIFY(created.metadataWritten);
        QString error;
        const auto information = storage->fileInformation(StoreSection::Files, "Brief #한글.txt", &error);
        QVERIFY2(!information.isEmpty(), qPrintable(error));
        QCOMPARE(information.value("format").toString(), "iiFileProvider.FileInformation");
        QCOMPARE(information.value("path").toString(), "files/Brief #한글.txt");
        QCOMPARE(information.value("details").toObject().value("mimeType").toString(), "text/plain");
        QCOMPARE(information.value("preview").toObject().value("kind").toString(), "text");
        QVERIFY(!information.value("preview").toObject().value("data").toString().isEmpty());
        const auto classification = information.value("classification").toObject();
        QCOMPARE(classification.value("status").toString(), "ready");
        QCOMPARE(classification.value("metadata").toObject().value("rating").toInt(), 5);
        QCOMPARE(classification.value("metadata").toObject().value("color").toString(), "blue");
        QCOMPARE(classification.value("collections").toArray().first().toObject().value("name").toString(), "Launch assets");
        QVERIFY(!information.contains("rootPath"));
        StorageDirectoryModel listing; listing.setFolder(QUrl::fromLocalFile(fixture.filePath("Files")));
        QTRY_COMPARE(listing.status(), StorageDirectoryModel::Ready);
        QCOMPARE(listing.rowCount(), 1);
        QCOMPARE(listing.get(0, "fileInformationProvider").toString(), "iiFileProvider");
        QCOMPARE(listing.get(0, "fileInformationKey").toString(), "files/Brief #한글.txt");
        write(fixture.filePath("Files/external.bin"), QByteArray("\0binary", 7));
        const auto external = storage->fileInformation(StoreSection::Files, "external.bin", &error);
        QVERIFY2(!external.isEmpty(), qPrintable(error));
        QCOMPARE(external.value("classification").toObject().value("status").toString(), "absent");
        write(fixture.filePath("Files/external.bin.iisacc.json"), "{\"rating\":99}");
        const auto malformed = storage->fileInformation(StoreSection::Files, "external.bin", &error);
        QCOMPARE(malformed.value("classification").toObject().value("status").toString(), "invalid");
        QVERIFY(!malformed.value("preview").toObject().value("data").toString().isEmpty());
        QVERIFY(storage->fileInformation(StoreSection::Files, "Brief #한글.txt.iisacc.json", &error).isEmpty());
        QVERIFY(storage->fileInformation(StoreSection::Files, "../Models/a", &error).isEmpty());
        QVERIFY(storage->fileInformation(StoreSection::Files, "", &error).isEmpty());
        QCOMPARE(iiFileProvider::native::File::read(root / std::filesystem::path(u8"Brief #한글.txt")), body);
        iiFileProvider::native::Presentation::cancelPending(std::filesystem::path(fixture.path().toStdU16String()));
    }
    void externalFilesReceiveProviderDetailsAndPreviewWithoutAnAppSave() {
        QTemporaryDir fixture(QDir::current().filePath("external-preview-XXXXXX.societycontainer")); QVERIFY(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        const auto registeredRoot = std::filesystem::path(drive->rootPath().toStdU16String());
        const auto maintenance = StorageMaintenance::runOnce(registeredRoot, drive->identifier().toStdString());
        QVERIFY2(maintenance.errors.empty(), maintenance.errors.empty() ? "" : maintenance.errors.front().c_str());
        const auto path = fixture.filePath("Files/External #한글.txt");
        const QByteArray contents = "A file copied by an external application";
        write(path, contents); // Intentional foreign writer fixture.
        const auto native = std::filesystem::path(path.toStdU16String());
        QTRY_VERIFY_WITH_TIMEOUT(iiFileProvider::native::Presentation::load(native).has_value(), 15000);
        const auto record = iiFileProvider::native::Presentation::load(native); QVERIFY(record);
        QCOMPARE(record->preview.kind, std::string("text"));
        QCOMPARE(record->details.size, std::uint64_t(contents.size()));
        QFile original(path); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), QByteArray("A file copied by an external application"));
        iiFileProvider::native::Presentation::cancelPending(std::filesystem::path(fixture.path().toStdU16String()));
    }
    void generatedPngPublicationPreservesBytesAndRejectsCollisionsAndRedirects() {
        QTemporaryDir fixture(QDir::current().filePath("publish-history-XXXXXX.societycontainer")); QVERIFY(fixture.isValid());
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto storage = SharedStorage::open(fixture.path()); QVERIFY(storage);
        const auto bytes = QByteArray::fromHex("89504e470d0a1a0a") + "original PNG payload";
        const auto source = fixture.filePath("Files/source.png"); write(source, bytes);
        QString error;
        const auto path = storage->publishGeneratedImage(source, "한글 #%-0001.PNG", &error);
        QCOMPARE(path, fixture.filePath("Generation History/한글 #%-0001.PNG")); QVERIFY(error.isEmpty());
        QFile output(path); QVERIFY(output.open(QIODevice::ReadOnly)); QCOMPARE(output.readAll(), bytes); output.close();
        QVERIFY(QFileInfo::exists(source));
        QVERIFY(storage->publishGeneratedImage(source, "한글 #%-0001.PNG", &error).isEmpty()); QVERIFY(!error.isEmpty());
        QVERIFY(output.open(QIODevice::ReadOnly)); QCOMPARE(output.readAll(), bytes); output.close();
        for (const auto &name : {"result.iisc", "../Files/result.png", "app/result.png", ".hidden.png", "result\\name.png"}) {
            QVERIFY(storage->publishGeneratedImage(source, name, &error).isEmpty()); QVERIFY(!error.isEmpty());
        }
        write(fixture.filePath("Files/invalid.png"), "native canvas or invalid PNG");
        QVERIFY(storage->publishGeneratedImage(fixture.filePath("Files/invalid.png"), "invalid.png", &error).isEmpty());
        QVERIFY(!QFileInfo::exists(fixture.filePath("Generation History/invalid.png")));
        const auto link = fixture.filePath("Files/link.png"); QVERIFY(createTestSymbolicLink(source, link));
        QVERIFY(storage->publishGeneratedImage(link, "link.png", &error).isEmpty());
        const QDir history(fixture.filePath("Generation History"));
        QVERIFY(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(15)));
        auto published = history.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        published.removeIf([](const QString& name) {
            return iiFileProvider::native::Presentation::isControlFile(std::filesystem::path(name.toStdU16String()));
        });
        QCOMPARE(published, QStringList{"한글 #%-0001.PNG"});
        QVERIFY(iiFileProvider::native::Presentation::load(std::filesystem::path(path.toStdU16String())));
        iiFileProvider::native::Presentation::cancelPending(std::filesystem::path(fixture.path().toStdU16String()));
        QVERIFY(iiFileProvider::File::remove(path));
        const auto lease = history.filePath(".iisacc-temporary.lock");
        if (QFileInfo::exists(lease)) { QCOMPARE(QFileInfo(lease).size(), 0); QVERIFY(QFile::remove(lease)); }
        // A published file now owns a durable classification index. Preserve
        // that fixture state while replacing the public section with a link.
        QVERIFY(QDir().rename(history.path(), fixture.filePath(".retained-history")));
        QVERIFY(createTestSymbolicLink(fixture.filePath("Files"), history.path()));
        QVERIFY(storage->publishGeneratedImage(source, "escape.png", &error).isEmpty());
        QVERIFY(!QFileInfo::exists(fixture.filePath("Files/escape.png")));
    }
    void runtimePathsStayInsideTheSelectedPackageAndCreateOnlyOnRequest()
    {
        QTemporaryDir fixture(QDir::current().filePath("runtime-boundary-XXXXXX.societycontainer")); QVERIFY(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        const auto storage = SharedStorage::open(fixture.path()); QVERIFY(storage);
        const auto runtime = fixture.filePath(".society-runtime");
        QCOMPARE(storage->runtimePath(), runtime);
        QCOMPARE(storage->runtimePath("state/helpers/session.json"), runtime + "/state/helpers/session.json");
        QVERIFY(!QFileInfo::exists(runtime));
        for (const auto &path : {"../Files", "a/../../Files", "a//b", "/absolute", "a\\b", "a:stream", ".", "a/"}) {
            QVERIFY(storage->runtimePath(path).isEmpty());
            QVERIFY(storage->ensureRuntimeDirectory(path).isEmpty());
        }
        QVERIFY(!QFileInfo::exists(runtime));
        QCOMPARE(storage->ensureRuntimeDirectory("state/helpers"), runtime + "/state/helpers");
        QVERIFY(!QFileInfo::exists(runtime + "/cache"));
        write(runtime + "/state/regular", "user bytes");
        QVERIFY(storage->runtimePath("state/regular/child").isEmpty());
        QVERIFY(storage->ensureRuntimeDirectory("state/regular").isEmpty());
        QVERIFY(createTestSymbolicLink(fixture.filePath("Files"), runtime + "/redirect"));
        QVERIFY(storage->runtimePath("redirect/escape").isEmpty());
        QVERIFY(storage->ensureRuntimeDirectory("redirect/escape").isEmpty());
        QVERIFY(QDir(fixture.filePath("Files")).isEmpty());
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto replica = SocietyDrive::adoptReplicaIdentity(fixture.path(), drive->identifier(), host); QVERIFY(replica);
        QVERIFY(storage->runtimePath("state").isEmpty());
        const auto owner = SharedStorage::open(fixture.path(), nullptr, true); QVERIFY(owner);
        QVERIFY(!owner->drive().isReady());
        QCOMPARE(owner->ensureRuntimeDirectory("sync"), runtime + "/sync");
        QVERIFY(!SharedStorage::open(fixture.path()));
    }
    void cachedDirectorySnapshotIsReconciledBeforeOpening() {
        QTemporaryDir fixture(QDir::current().filePath("directory-cache-XXXXXX.societycontainer")); QVERIFY(fixture.isValid());
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto files = fixture.filePath("Files");
        write(QDir(files).filePath("old.txt"), "original");
        {
            StorageDirectoryModel initial; initial.setFolder(QUrl::fromLocalFile(files));
            QTRY_COMPARE(initial.status(), StorageDirectoryModel::Ready); QCOMPARE(initial.rowCount(), 1);
            // The snapshot write is deliberately outside the visible Ready path.
            const auto persisted = [&] {
                const QDir cache(QDir(StorageDirectoryModel::cacheDirectory(files)).filePath("directories-v1"));
                for (const auto &path : cache.entryList({"*.bin"}, QDir::Files)) {
                    QFile file(cache.filePath(path)); if (!file.open(QIODevice::ReadOnly)) continue;
                    QDataStream stream(&file); stream.setVersion(QDataStream::Qt_6_8);
                    quint32 version; QString binding; QList<QVariantMap> rows;
                    stream >> version >> binding >> rows;
                    if (stream.status() == QDataStream::Ok && rows.size() == 1
                        && rows[0].value("filePath") == QDir(files).filePath("old.txt")) return true;
                }
                return false;
            };
            QTRY_VERIFY(persisted());
        }
        QVERIFY(QFile::remove(QDir(files).filePath("old.txt"))); write(QDir(files).filePath("new.txt"), "current");
        StorageDirectoryModel restored;
        QSignalSpy cached(&restored, &StorageDirectoryModel::snapshotRestored);
        QSignalSpy activated(&restored, &StorageDirectoryModel::activated);
        connect(&restored, &StorageDirectoryModel::snapshotRestored, &restored, [&] {
            QCOMPARE(restored.status(), StorageDirectoryModel::Loading);
            QCOMPARE(restored.get(0, "fileName").toString(), "old.txt");
            restored.activate(0); QCOMPARE(activated.size(), 0);
        });
        restored.setFolder(QUrl::fromLocalFile(files));
        QTRY_COMPARE(restored.status(), StorageDirectoryModel::Ready);
        QCOMPARE(cached.size(), 1); QCOMPARE(restored.rowCount(), 1);
        QCOMPARE(restored.get(0, "fileName").toString(), "new.txt");
        QCOMPARE(restored.entry(0).value("filePath"), restored.get(0, "filePath"));
        QVERIFY(restored.entry(-1).isEmpty());
        QVERIFY(restored.entry(restored.rowCount()).isEmpty());
        write(QDir(files).filePath("watched.txt"), "observed");
        QTRY_COMPARE_WITH_TIMEOUT(restored.rowCount(), 2, 3000); // No explicit refresh or ten-second fallback.
    }
    void generationInventoryReconcilesAuthorityAndPreservesReplicas() {
        QTemporaryDir fixture(QDir::current().filePath("generation-inventory-XXXXXX.societycontainer")); QVERIFY(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        StorageMap map(*drive);
        const QJsonObject old{{"path", "models/Checkpoint/old.safetensors"}, {"kind", "file"},
            {"size", "7"}, {"resident", true}, {"version", QString(64, 'a')}};
        QVERIFY(QDir().mkdir(fixture.filePath("Models/Checkpoint")));
        write(fixture.filePath("Models/Checkpoint/old.safetensors"), "fixture");
        QVERIFY(map.publish({old}));
        auto storage = SharedStorage::open(fixture.path()); QVERIFY(storage);
        const auto reference = storage->models().first().reference(drive->identifier());
        write(fixture.filePath(".society-sync/primary.json"), QJsonDocument(QJsonObject{
            {"schema", 1}, {"container", drive->identifier()}, {"scope", QString(64, 'a')}, {"host", "test-host"}}).toJson());
        QCOMPARE(storage->models().first().reference(drive->identifier()), reference);
        QVERIFY(FileOperations(*drive).perform(FileOperations::Action::Trash,
            fixture.filePath("Models/Checkpoint/old.safetensors")));
        write(fixture.filePath("Models/Checkpoint/new.safetensors"), "new weights");
        QVERIFY(QDir().mkpath(fixture.filePath("Models/Checkpoint/package")));
        write(fixture.filePath("Models/Checkpoint/package/model_index.json"), "{}");
        write(fixture.filePath("Models/Checkpoint/package/model.safetensors"), "package weights");
        QVERIFY(QDir().mkpath(fixture.filePath("Models/.society-runtime")));
        write(fixture.filePath("Models/.society-runtime/hidden.safetensors"), "hidden");
        QVERIFY(createTestSymbolicLink(fixture.filePath("Deleted/old.safetensors"), fixture.filePath("Models/Checkpoint/link.safetensors")));
        const auto models = storage->models(); QCOMPARE(models.size(), 2);
        QCOMPARE(models[0].id, "Checkpoint/new.safetensors"); QVERIFY(models[0].available);
        QCOMPARE(models[0].bytes, 11);
        QCOMPARE(models[1].id, "Checkpoint/package"); QCOMPARE(models[1].format, "diffusers");
        QVERIFY(storage->resolveModel(reference).isEmpty());
        for (const auto &model : models)
            QCOMPARE(storage->resolveModel(model.reference(drive->identifier())), fixture.filePath("Models/" + model.id));
        QCOMPARE(map.objects(), QJsonArray({old})); QVERIFY(map.pendingRequests().isEmpty());
        // Replicas retain remote, nonresident models and never resurrect local leftovers.
        QVERIFY(QFile::remove(fixture.filePath(".society-sync/primary.json")));
        makeReadyReplica(*drive);
        const auto remote = storage->models(); QCOMPARE(remote.size(), 1);
        QCOMPARE(remote.first().id, "Checkpoint/old.safetensors"); QVERIFY(!remote.first().available);
        QVERIFY(map.publish({})); QVERIFY(storage->models().isEmpty());
    }

    void originalModelFilesRemainVisibleWithAPartialCatalogAndNoRegisteredHost() {
        QTemporaryDir fixture(QDir::current().filePath("original-model-catalog-XXXXXX.societycontainer")); QVERIFY(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        StorageMap map(*drive);
        const QJsonArray partial{
            QJsonObject{{"path", "models/Checkpoint"}, {"kind", "directory"}},
            QJsonObject{{"path", "models/LoRA"}, {"kind", "directory"}}};
        QVERIFY(map.publish(partial));
        QStringList paths;
        for (const auto &category : {QString("Checkpoint"), QString("LoRA")}) {
            QVERIFY(QDir().mkdir(fixture.filePath("Models/" + category)));
            const int count = category == "Checkpoint" ? 2 : 6;
            for (int i = 0; i < count; ++i) {
                const auto path = fixture.filePath(QString("Models/%1/user-%2.safetensors").arg(category).arg(i));
                write(path, "user weights"); paths.append(path);
            }
        }
        QVERIFY(!QFileInfo::exists(fixture.filePath(".society-sync/primary.json")));
        QVERIFY(map.isLocalAuthority());
        StorageModelCatalog catalog; catalog.setDirectory(fixture.filePath("Models"));
        QTRY_COMPARE(catalog.count(), 8);
        QCOMPARE(catalog.groups().value("Checkpoint").toList().size(), 2);
        QCOMPARE(catalog.groups().value("LoRA").toList().size(), 6);
        const auto storage = SharedStorage::open(fixture.path()); QVERIFY(storage);
        const auto inventory = storage->models(); QCOMPARE(inventory.size(), 8);
        for (const auto &model : inventory) {
            QVERIFY(model.available);
            QVERIFY(paths.contains(storage->resolveModel(model.reference(drive->identifier()))));
        }
        QCOMPARE(map.objects(), partial); QVERIFY(map.pendingRequests().isEmpty());
        QVERIFY(!QFileInfo::exists(fixture.filePath(".society-sync/primary.json")));
        for (const auto &path : paths) {
            QFile source(path); QVERIFY(source.open(QIODevice::ReadOnly));
            QCOMPARE(source.readAll(), QByteArray("user weights"));
        }
    }

    void replicaCatalogDoesNotExposeUnindexedLocalModels_data() {
        QTest::addColumn<bool>("complete");
        QTest::newRow("incomplete") << false;
        QTest::newRow("complete") << true;
    }
    void replicaCatalogDoesNotExposeUnindexedLocalModels() {
        QFETCH(bool, complete);
        QTemporaryDir fixture(QDir::current().filePath("replica-model-catalog-XXXXXX.societycontainer")); QVERIFY(fixture.isValid());
        const auto original = SocietyDrive::create(fixture.path()); QVERIFY(original);
        QVERIFY(QDir().mkdir(fixture.filePath("Models/Checkpoint")));
        write(fixture.filePath("Models/Checkpoint/leftover.safetensors"), "unindexed cache");
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto replica = SocietyDrive::adoptReplicaIdentity(fixture.path(), original->identifier(), host); QVERIFY(replica);
        if (complete) QVERIFY(SocietyDrive::completeReplica(fixture.path(), host));
        StorageMap map(*replica); QVERIFY(map.publish({})); QVERIFY(!map.isLocalAuthority());
        StorageModelCatalog catalog; catalog.setDirectory(fixture.filePath("Models"));
        QTRY_VERIFY(!catalog.loading()); QCOMPARE(catalog.count(), 0);
        const auto storage = SharedStorage::open(fixture.path(), nullptr, true); QVERIFY(storage);
        QVERIFY(storage->models().isEmpty());
        QVERIFY(QFileInfo::exists(fixture.filePath("Models/Checkpoint/leftover.safetensors")));
        QVERIFY(map.pendingRequests().isEmpty());
        QVERIFY(!QFileInfo::exists(fixture.filePath(".society-sync/primary.json")));
    }

    void modelCatalogIgnoresUnchangedRefreshAndDocumentation_data() {
        QTest::addColumn<bool>("synchronized");
        QTest::newRow("filesystem") << false;
        QTest::newRow("replica-catalog") << true;
    }
    void modelCatalogIgnoresUnchangedRefreshAndDocumentation() {
        QFETCH(bool, synchronized);
        QTemporaryDir fixture(QDir::current().filePath("model-refresh-XXXXXX.societycontainer")); QVERIFY(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        QJsonArray objects;
        const auto add = [&](const QString &relative, const QByteArray &bytes) {
            const auto path = fixture.filePath("Models/" + relative);
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath())); write(path, bytes);
            objects.append(QJsonObject{{"path", "models/" + relative}, {"kind", "file"},
                {"size", QString::number(bytes.size())}, {"resident", false}});
        };
        add("VAE/decoder.safetensors", "fixture weights");
        add("VAE/README.md", "documentation"); add("VAE/LICENSE", "license");
        add("VAE/NOTICE.txt", "notice"); add("VAE/config.json", "{}");
        add("Wildcards/colors.txt", "red\nblue");
        StorageMap map(*drive);
        if (synchronized) makeReadyReplica(*drive);
        if (synchronized) QVERIFY(map.publish(objects));
        StorageModelCatalog catalog;
        catalog.setDirectory(fixture.filePath("Models"));
        // The model scans a real filesystem asynchronously; external volumes can
        // take several seconds to publish even this small fixture under I/O load.
        QTRY_VERIFY_WITH_TIMEOUT(!catalog.loading(), 30000);
        QCOMPARE(catalog.groups().value("VAE").toList().size(), 1);
        QCOMPARE(catalog.groups().value("Wildcards").toList().size(), 1);
        QCOMPARE(catalog.count(), 2);
        const auto snapshot = catalog.groups();
        QSignalSpy changes(&catalog, &StorageModelCatalog::modelsChanged);
        QSignalSpy before(&catalog, &StorageModelCatalog::modelsAboutToChange);
        QSignalSpy loading(&catalog, &StorageModelCatalog::loadingChanged);
        for (int i = 0; i < 4; ++i) {
            if (synchronized) QVERIFY(map.publish(objects)); // Same rows, rewritten catalog.
            else write(fixture.filePath("Models/VAE/README.md"), QByteArray(i + 1, 'x'));
            catalog.refresh(); catalog.refresh();
            QTest::qWait(250);
        }
        QCOMPARE(catalog.groups(), snapshot);
        QCOMPARE(changes.size(), 0); QCOMPARE(before.size(), 0); QCOMPARE(loading.size(), 0);
        add("VAE/new.bin", "new weights");
        if (synchronized) QVERIFY(map.publish(objects));
        catalog.refresh();
        QTRY_COMPARE(catalog.count(), 3);
        QCOMPARE(loading.size(), 0);
        QVERIFY(changes.size() > 0);
        QVERIFY(map.pendingRequests().isEmpty());
    }

    void hostModelImportsAppearBeforeSyncHashingCompletes() {
        QTemporaryDir fixture(QDir::current().filePath("host-model-import-XXXXXX.societycontainer")); QVERIFY(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        makeReadyReplica(*drive);
        StorageMap map(*drive); QVERIFY(map.publish({}));
        const auto source = fixture.filePath("Models/Other/krea.safetensors");
        QVERIFY(QDir().mkdir(fixture.filePath("Models/Other")));
        write(source, "metadata-only fixture");
        StorageModelCatalog catalog; catalog.setDirectory(fixture.filePath("Models"));
        QTRY_VERIFY(!catalog.loading()); QCOMPARE(catalog.count(), 0); // Replica remains catalog-only.
        write(fixture.filePath(".society-sync/primary.json"), QJsonDocument(QJsonObject{
            {"schema", 1}, {"container", drive->identifier()}, {"scope", QString(64, 'a')}, {"host", "test-host"}}).toJson());
        catalog.refresh(); QTRY_COMPARE(catalog.count(), 1);
        const auto destination = fixture.filePath("Models/Checkpoint/krea.safetensors");
        QVERIFY(QDir().mkdir(fixture.filePath("Models/Checkpoint")));
        QVERIFY(QFile::rename(source, destination));
        catalog.refresh(); QTRY_COMPARE(catalog.groups().value("Checkpoint").toList().size(), 1);
        QCOMPARE(catalog.groups().value("Other").toList().size(), 0);
        const auto row = catalog.groups().value("Checkpoint").toList().first().toMap();
        QCOMPARE(row.value("bytes").toLongLong(), QFileInfo(destination).size()); QVERIFY(row.value("available").toBool());
        QTest::qWait(300);
        QSignalSpy changed(&catalog, &StorageModelCatalog::modelsChanged);
        for (int i = 0; i < 3; ++i) { catalog.refresh(); QTest::qWait(200); }
        QCOMPARE(changed.size(), 0); QVERIFY(map.objects().isEmpty()); QVERIFY(map.pendingRequests().isEmpty());
    }
    void modelTypesMatchPhysicalFoldersAndRemoteCatalog() {
        QTemporaryDir fixture(QDir::current().filePath("model-types-XXXXXX.societycontainer")); QVERIFY(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        QVERIFY(QDir(fixture.filePath("Models")).isEmpty());
        QJsonArray objects;
        int expected = 0;
        const auto add = [&](const QString &relative, const QByteArray &bytes) {
            const auto path = fixture.filePath("Models/" + relative);
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            write(path, bytes);
            objects.append(QJsonObject{{"path", "models/" + relative}, {"kind", "file"},
                {"size", QString::number(bytes.size())}, {"resident", false}, {"version", QString(64, 'a')}});
        };
        for (const auto type : allModelTypes()) {
            const auto name = modelTypeName(type);
            const auto extension = type == ModelType::Wildcards ? ".txt"
                : type == ModelType::Workflows || type == ModelType::ComfyUIWorkflows || type == ModelType::Poses ? ".json" : ".bin";
            add(name + "/sample" + extension, "{}"); ++expected;
            QVERIFY(QFileInfo(fixture.filePath("Models/" + name)).isDir());
        }
        add("Checkpoint/sample.bin.model.json", "{\"society.modality\":\"audio\"}");
        add("Checkpoint/sample.preview.png", "preview");
        add("LLM/package/config.json", "{\"architectures\":[\"LlamaForCausalLM\"]}");
        add("LLM/package/model.bin", "weights"); ++expected;
        add("Checkpoint/pipeline/model_index.json", "{}");
        add("Checkpoint/pipeline/unet/model.bin", "weights"); ++expected;
        add("legacy/unassigned.gguf", "unreadable"); ++expected;
        add("Other/.hidden.bin", "hidden");
        StorageModelCatalog local; local.setDirectory(fixture.filePath("Models"));
        QTRY_VERIFY(!local.loading());
        QCOMPARE(local.count(), expected);
        QCOMPARE(local.categories().size(), 23);
        QCOMPARE(local.groups().size(), 23);
        QCOMPARE(local.uncategorizedCount(), 2);
        for (const auto type : allModelTypes()) {
            const auto name = modelTypeName(type);
            QCOMPARE(local.groups().value(name).toList().size(),
                type == ModelType::Checkpoint || type == ModelType::LLM || type == ModelType::Other ? 2 : 1);
        }
        StorageMap map(*drive); QVERIFY(map.publish(objects));
        // Replicas must use folder identity even with all payloads absent.
        makeReadyReplica(*drive);
        QVERIFY(!map.isLocalAuthority());
        QVERIFY(QDir(fixture.filePath("Models")).removeRecursively());
        QVERIFY(QDir().mkpath(fixture.filePath("Models")));
        StorageModelCatalog remote; remote.setDirectory(fixture.filePath("Models"));
        QTRY_VERIFY(!remote.loading());
        QCOMPARE(remote.count(), expected);
        QCOMPARE(remote.uncategorizedCount(), 2);
        for (const auto type : allModelTypes()) {
            const auto name = modelTypeName(type);
            QCOMPARE(remote.groups().value(name).toList().size(), local.groups().value(name).toList().size());
            for (const auto &value : remote.groups().value(name).toList()) {
                const auto row = value.toMap();
                QVERIFY(!QFileInfo::exists(row.value("path").toString()));
                QCOMPARE(row.value("type").toString(), name);
            }
        }
        QVERIFY(map.pendingRequests().isEmpty());
    }

    void directoryRefreshPublishesOnlyTheChangedRows() {
        QTemporaryDir fixture(QDir::current().filePath("directory-diff-XXXXXX.societycontainer")); QVERIFY(fixture.isValid());
        write(fixture.filePath("b.txt"), "b");
        write(fixture.filePath("c.txt"), "cc");
        StorageDirectoryModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.setFolder(QUrl::fromLocalFile(fixture.path()));
        QTRY_COMPARE(model.status(), StorageDirectoryModel::Ready);
        QCOMPARE(model.rowCount(), 2);
        const auto roles = model.roleNames();
        const int pathRole = roles.key("filePath"), sizeRole = roles.key("fileSize");
        QPersistentModelIndex selected(model.index(1));
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
        QSignalSpy counts(&model, &StorageDirectoryModel::countChanged);
        QSignalSpy status(&model, &StorageDirectoryModel::statusChanged);

        // Both explicit checks and two automatic polls must remain invisible.
        for (int i = 0; i < 3; ++i) model.refresh();
        QTest::qWait(2200);
        QCOMPARE(resets.size(), 0); QCOMPARE(changed.size(), 0);
        QCOMPARE(inserted.size(), 0); QCOMPARE(removed.size(), 0); QCOMPARE(moved.size(), 0);
        QCOMPARE(counts.size(), 0); QCOMPARE(status.size(), 0);

        write(fixture.filePath("b.txt"), "longer content"); model.refresh();
        QTRY_COMPARE(model.get(0, "fileSize").toLongLong(), 14);
        QCOMPARE(resets.size(), 0);
        QCOMPARE(changed.size(), 1); QCOMPARE(counts.size(), 0);
        QCOMPARE(changed.first().at(0).value<QModelIndex>().row(), 0);
        QVERIFY(changed.first().at(2).value<QList<int>>().contains(sizeRole));
        QCOMPARE(selected.data(pathRole).toString(), fixture.filePath("c.txt"));

        write(fixture.filePath("a.txt"), "a"); model.refresh();
        QTRY_COMPARE(model.rowCount(), 3);
        QCOMPARE(resets.size(), 0); QCOMPARE(inserted.size(), 1); QCOMPARE(counts.size(), 1);
        QCOMPARE(selected.row(), 2);
        QCOMPARE(selected.data(pathRole).toString(), fixture.filePath("c.txt"));
        QVERIFY(QFile::remove(fixture.filePath("b.txt"))); model.refresh();
        QTRY_COMPARE(model.rowCount(), 2);
        QCOMPARE(resets.size(), 0); QCOMPARE(removed.size(), 1); QCOMPARE(counts.size(), 2);
        QCOMPARE(selected.row(), 1);

        QVERIFY(model.setProperty("sortReversed", true));
        QTRY_COMPARE(model.get(0, "fileName").toString(), "c.txt");
        QCOMPARE(resets.size(), 0); QVERIFY(!moved.isEmpty()); QCOMPARE(counts.size(), 2);
        QCOMPARE(selected.row(), 0);
        QCOMPARE(selected.data(pathRole).toString(), fixture.filePath("c.txt"));
        QVERIFY(QFile::remove(fixture.filePath("c.txt"))); model.refresh();
        QTRY_COMPARE(model.rowCount(), 1); QVERIFY(!selected.isValid());
        QCOMPARE(resets.size(), 0);
    }
    void hostListingsFollowDeletionBeforeTheCatalogCatchesUp() {
        QTemporaryDir fixture(QDir::current().filePath("host-delete-listing-XXXXXX.societycontainer")); QVERIFY(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive); StorageMap map(*drive);
        const auto source = fixture.filePath("Models/Checkpoint/anima.safetensors");
        QVERIFY(QDir().mkdir(fixture.filePath("Models/Checkpoint")));
        write(source, "fixture");
        const QJsonObject model{{"path", "models/Checkpoint/anima.safetensors"}, {"kind", "file"},
            {"size", "7"}, {"resident", true}, {"version", QString(64, 'a')}};
        QVERIFY(map.publish({model}));
        write(fixture.filePath(".society-sync/primary.json"), QJsonDocument(QJsonObject{
            {"schema", 1}, {"container", drive->identifier()}, {"scope", QString(64, 'a')}, {"host", "test-host"}}).toJson());
        StorageModelCatalog catalog; catalog.setDirectory(fixture.filePath("Models"));
        StorageDirectoryModel original; original.setFolder(QUrl::fromLocalFile(fixture.filePath("Models/Checkpoint")));
        QTRY_COMPARE(catalog.count(), 1); QTRY_COMPARE(original.rowCount(), 1);
        const auto moved = FileOperations(*drive).perform(FileOperations::Action::Trash, source); QVERIFY(moved);
        catalog.refresh(); original.refresh();
        QTRY_COMPARE(catalog.count(), 0); QTRY_COMPARE(original.rowCount(), 0);
        // Even an old tombstone cannot hide a newly moved file on the authority.
        const QJsonObject tombstone{{"path", "deleted/anima.safetensors"}, {"kind", "deleted"}};
        QVERIFY(map.publish({model, tombstone}));
        StorageDirectoryModel deleted; deleted.setFolder(QUrl::fromLocalFile(fixture.filePath("Deleted")));
        QTRY_COMPARE(deleted.rowCount(), 1);
        auto stale = model; stale["path"] = "deleted/anima.safetensors"; QVERIFY(map.publish({model, stale}));
        deleted.refresh(); QTRY_COMPARE(deleted.rowCount(), 1);
        QVERIFY(FileOperations(*drive).perform(FileOperations::Action::Remove, moved.path));
        deleted.refresh(); QTRY_COMPARE(deleted.rowCount(), 0);
        QVERIFY(map.pendingRequests().isEmpty());
    }
    void largeDirectoryRefreshReturnsBeforeReadingTheStorageMap() {
        QTemporaryDir fixture(QDir::current().filePath("large-directory-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        makeReadyReplica(*drive);
        StorageMap map(*drive); QJsonArray objects;
        for (int i = 0; i < 8000; ++i)
            objects.append(QJsonObject{{"path", QString("models/Checkpoint/remote-%1.safetensors").arg(i)},
                {"kind", "file"}, {"size", "9000000000"}, {"resident", false}});
        objects.append(QJsonObject{{"path", "files/Documents/visible.txt"}, {"kind", "file"},
            {"size", "123"}, {"resident", false}});
        QVERIFY(map.publish(objects));
        QVERIFY(QDir().mkpath(fixture.filePath("Files/Documents")));
        StorageDirectoryModel model; model.setFolder(QUrl::fromLocalFile(fixture.filePath("Files/Documents")));
        QElapsedTimer elapsed; elapsed.start(); model.refresh();
        QVERIFY2(elapsed.elapsed() < 50, "Directory refresh blocked its GUI caller on the full catalog");
        QTRY_COMPARE(model.rowCount(), 1);
        QCOMPARE(model.get(0, "fileName").toString(), "visible.txt");
        QVERIFY(map.pendingRequests().isEmpty());
        QVERIFY(!QFileInfo::exists(fixture.filePath("Files/Documents/visible.txt")));
    }
    void directoryListsRemoteObjectsAndOpensOnlyCompletePayloads() {
        QTemporaryDir fixture(QDir::current().filePath("remote-directory-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        makeReadyReplica(*drive);
        StorageMap map(*drive);
        QJsonObject object{{"path", "files/Documents/remote.png"}, {"kind", "file"},
            {"size", "3"}, {"version", QString(64, 'a')}, {"hash", QString(64, 'b')}, {"resident", false}};
        QVERIFY(map.publish({object}));
        QVERIFY(QDir().mkpath(fixture.filePath("Files/Documents")));
        StorageDirectoryModel model; model.setFolder(QUrl::fromLocalFile(fixture.filePath("Files/Documents")));
        QTRY_COMPARE(model.rowCount(), 1);
        QCOMPARE(model.get(0, "fileName").toString(), "remote.png");
        QVERIFY(!model.get(0, "fileResident").toBool());
        QVERIFY(model.get(0, "filePreviewUrl").toUrl().isEmpty());
        const auto path = map.localPath("files/Documents/remote.png"); QVERIFY(!QFileInfo::exists(path));
        QSignalSpy opened(&model, &StorageDirectoryModel::activated);
        model.activate(0); QCOMPARE(opened.size(), 0); QTRY_COMPARE(map.pendingRequests().size(), 1);
        const auto id = map.pendingRequests().first().toObject().value("id").toString();
        write(path, "png"); object["resident"] = true; QVERIFY(map.publish({object}));
        QVERIFY(map.finishRequest(id)); model.refresh(); QTRY_COMPARE(opened.size(), 1);
        QCOMPARE(opened.first().first().toString(), path); QTRY_VERIFY(model.get(0, "fileResident").toBool());
        QVERIFY(QFile::remove(path)); model.refresh(); QTRY_VERIFY(!model.get(0, "fileResident").toBool());
        QCOMPARE(model.rowCount(), 1); // Cache absence does not erase the host row.
    }
    void navigationDiscardsAStaleDirectoryRead() {
        QTemporaryDir fixture(QDir::current().filePath("directory-navigation-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        QVERIFY(QDir().mkpath(fixture.filePath("Files/Documents")));
        QVERIFY(QDir().mkpath(fixture.filePath("Files/Audios")));
        write(fixture.filePath("Files/Documents/document.txt"), "doc");
        write(fixture.filePath("Files/Audios/audio.wav"), "audio");
        StorageDirectoryModel model;
        model.setFolder(QUrl::fromLocalFile(fixture.filePath("Files/Documents"))); model.refresh();
        model.setFolder(QUrl::fromLocalFile(fixture.filePath("Files/Audios"))); model.refresh();
        QTRY_COMPARE(model.rowCount(), 1);
        QTRY_COMPARE(model.get(0, "fileName").toString(), "audio.wav");
        QTest::qWait(100); QCOMPARE(model.get(0, "fileName").toString(), "audio.wav");
        model.setFolder({}); QCOMPARE(model.rowCount(), 0); QCOMPARE(model.status(), StorageDirectoryModel::Null);
    }
    void modelCardsCreateOnlyTheSelectedDownloadRequest() {
        QTemporaryDir fixture(QDir::current().filePath("model-cards-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        makeReadyReplica(*drive);
        StorageMap map(*drive);
        QJsonObject selected{{"path", "models/Checkpoint/selected.safetensors"}, {"kind", "file"},
            {"size", "3"}, {"resident", false}, {"version", QString(64, 'a')}};
        auto other = selected; other["path"] = "models/LoRA/unselected.safetensors";
        QVERIFY(map.publish({selected, other}));
        StorageModelCatalog catalog; catalog.setDirectory(fixture.filePath("Models"));
        QTRY_COMPARE(catalog.count(), 2); QTRY_VERIFY(!catalog.loading());
        catalog.refresh(); QTRY_VERIFY(!catalog.loading()); QVERIFY(map.pendingRequests().isEmpty());
        const auto path = fixture.filePath("Models/Checkpoint/selected.safetensors");
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(fixture.filePath("Models/Checkpoint")));
        QSignalSpy opened(&catalog, &StorageModelCatalog::objectReady);
        QElapsedTimer elapsed; elapsed.start(); catalog.activatePath(path, true);
        QVERIFY(elapsed.elapsed() < 50); QTRY_COMPARE(map.pendingRequests().size(), 1);
        const auto request = map.pendingRequests().first().toObject();
        const auto requested = request.value("objects").toArray(); QCOMPARE(requested.size(), 1);
        QCOMPARE(requested.first().toObject().value("path"), selected.value("path"));
        QCOMPARE(requested.first().toObject().value("version"), selected.value("version"));
        QVERIFY(!QFileInfo::exists(fixture.filePath("Models/LoRA/unselected.safetensors")));
        QVERIFY(!QFileInfo::exists(fixture.filePath("Models/LoRA")));
        QVERIFY(QDir().mkdir(fixture.filePath("Models/Checkpoint")));
        write(path, "abc"); selected["resident"] = true; QVERIFY(map.publish({selected, other}));
        QVERIFY(map.finishRequest(request.value("id").toString()));
        QTRY_COMPARE(opened.size(), 1); QCOMPARE(opened.first().first().toString(), path);
        QTRY_COMPARE(catalog.downloadStatus(), QString("Available on this device"));
    }
    void missingModelPackageIsNotReportedReady() {
        QTemporaryDir fixture(QDir::current().filePath("missing-package-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        StorageDirectoryModel opener;
        QSignalSpy ready(&opener, &StorageDirectoryModel::activated);
        QSignalSpy failed(&opener, &StorageDirectoryModel::downloadFailed);
        opener.openPath(fixture.filePath("Models/Checkpoint/removed-package"), true);
        QTRY_COMPARE(failed.size(), 1); QCOMPARE(ready.size(), 0);
    }
    void remoteModelCatalogDoesNotCreatePayloads() {
        QTemporaryDir fixture(QDir::current().filePath("remote-models-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path()); QVERIFY(drive);
        makeReadyReplica(*drive);
        StorageMap map(*drive);
        const QJsonObject object{{"path", "models/Checkpoint/remote.safetensors"}, {"kind", "file"},
            {"size", "9000000000"}, {"version", QString(64, 'a')}, {"hash", QString(64, 'b')}, {"resident", false}};
        QVERIFY(map.publish({object}));
        const auto storage = SharedStorage::open(fixture.path()); QVERIFY(storage);
        const auto models = storage->models(); QCOMPARE(models.size(), 1);
        QCOMPARE(models.first().id, "Checkpoint/remote.safetensors"); QVERIFY(!models.first().available);
        QVERIFY(!QFileInfo::exists(map.localPath("models/Checkpoint/remote.safetensors")));
        QString error; QVERIFY(storage->resolveModel(models.first().reference(drive->identifier()), &error).isEmpty());
        QVERIFY(error.contains("Download"));
        QVERIFY(map.request({"models/../outside"}, &error).isEmpty());
        const auto id = map.request({"models/Checkpoint/remote.safetensors"}); QVERIFY(!id.isEmpty());
        QCOMPARE(map.pendingRequests().size(), 1); QVERIFY(map.cancel(id)); QVERIFY(map.pendingRequests().isEmpty());
    }
    void defaultSelectionFollowsHostAdoptionAndConsumersWaitForReadiness()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-mirror-XXXXXX.societycontainer"));
        const auto original = SocietyDrive::create(fixture.path()); QVERIFY(original);
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", fixture.filePath("settings.json").toUtf8());
        qunsetenv("SOCIETY_CONTAINER_PATH");
        QVERIFY(SharedStorage::setDefaultContainer(fixture.path()));
        const auto consumer = SharedStorage::open(); QVERIFY(consumer);
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto mirror = SocietyDrive::adoptReplicaIdentity(fixture.path(), original->identifier(), host); QVERIFY(mirror);
        QVERIFY(mirror->isValid()); QVERIFY(!mirror->isReady());
        QVERIFY(!SharedStorage::open());
        const auto owner = SharedStorage::open({}, nullptr, true); QVERIFY(owner);
        QCOMPARE(owner->drive().identifier(), host);
        QVERIFY(owner->filePath(StoreSection::Files, "pending").isEmpty());
        QVERIFY(owner->ensureDirectory(StoreSection::Files, "pending").isEmpty());
        QVERIFY(consumer->filePath(StoreSection::Files, "stale").isEmpty());
        QVERIFY(SocietyDrive::completeReplica(fixture.path(), host));
        const auto reopened = SharedStorage::open(); QVERIFY(reopened); QCOMPARE(reopened->drive().identifier(), host);
        QVERIFY(SharedStorage::setDefaultContainer(fixture.path()));
        const auto replacement = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(SocietyDrive::adoptReplicaIdentity(fixture.path(), host, replacement));
        QVERIFY(SharedStorage::open({}, nullptr, true));
        QVERIFY(SocietyDrive::completeReplica(fixture.path(), replacement));
        QCOMPARE(SharedStorage::open()->drive().identifier(), replacement);
    }
    void rejectsSavedImageSettingsWithoutChangingFilesOrConfiguration()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-legacy-settings-XXXXXX.societycontainer"));
        QVERIFY(fixture.isValid());
        const auto drive = SocietyDrive::createAt(fixture.path()); QVERIFY(drive);
        const auto previousSettings = qgetenv("SOCIETY_STORAGE_SETTINGS_PATH");
        const auto previousContainer = qgetenv("SOCIETY_CONTAINER_PATH");
        const auto restore = qScopeGuard([&] {
            qputenv("SOCIETY_STORAGE_SETTINGS_PATH", previousSettings);
            qputenv("SOCIETY_CONTAINER_PATH", previousContainer);
        });
        const auto settingsPath = fixture.filePath("settings.json");
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", settingsPath.toUtf8());
        qunsetenv("SOCIETY_CONTAINER_PATH");
        const auto imagePath = fixture.filePath("Past.societycontainer");
        QVERIFY(QDir().mkpath(imagePath + "/bands"));
        write(imagePath + "/bands/preserved", "legacy bytes");
        for (const int version : {1, 2}) {
            const auto configuration = QJsonDocument(QJsonObject{{"schemaVersion", version},
                {"imagePath", imagePath}, {"path", drive->rootPath()},
                {"containerId", drive->identifier()}}).toJson();
            write(settingsPath, configuration);
            QString error;
            QVERIFY(!SharedStorage::open({}, &error));
            QVERIFY(error.contains("not restored"));
            QFile settings(settingsPath); QVERIFY(settings.open(QIODevice::ReadOnly));
            QCOMPARE(settings.readAll(), configuration);
            QFile payload(imagePath + "/bands/preserved"); QVERIFY(payload.open(QIODevice::ReadOnly));
            QCOMPARE(payload.readAll(), QByteArray("legacy bytes"));
        }
        // A valid ordinary folder remains available through an explicit selection.
        QVERIFY(SharedStorage::setDefaultContainer(drive->rootPath()));
        const auto selected = SharedStorage::open(); QVERIFY(selected);
        QCOMPARE(selected->drive().identifier(), drive->identifier());
    }

    void rejectsMisplacedContainersWithoutReplacingSharedSettings()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-nested-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path());
        QVERIFY(drive);
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", fixture.filePath("settings.json").toUtf8());
        qunsetenv("SOCIETY_CONTAINER_PATH");
        QVERIFY(SharedStorage::setDefaultContainer(fixture.path()));
        const auto nested = fixture.filePath("Files");
        for (const auto section : allStoreSections())
            QVERIFY(QDir().mkdir(QDir(nested).filePath(storeSectionName(section))));
        QVERIFY(QFile::copy(fixture.filePath(".society-drive.json"), QDir(nested).filePath(".society-drive.json")));
        QString error;
        QVERIFY(!SharedStorage::setDefaultContainer(nested, &error));
        QVERIFY(!SharedStorage::open(nested, &error));
        const auto unchanged = SharedStorage::open();
        QVERIFY(unchanged);
        QCOMPARE(unchanged->drive().rootPath(), fixture.path());
        QVERIFY(unchanged->filePath(StoreSection::Models, "private.safetensors").startsWith(fixture.filePath("Models/")));
    }

    void nativePathsSupportOrdinaryFilesInEverySection()
    {
        QTemporaryDir fixture(QDir::current().filePath("native-files-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path());
        QVERIFY(drive);
        const auto storage = SharedStorage::open(fixture.path());
        QVERIFY(storage);
        QString error;
        for (const auto section : allStoreSections()) {
            QCOMPARE(storage->filePath(section, {}, &error), drive->sectionPath(section));
            const auto directory = storage->ensureDirectory(section, "Client/.state", &error);
            QVERIFY2(!directory.isEmpty(), qPrintable(error));
            const auto path = storage->filePath(section, "Client/.state/한글 #%.bin", &error);
            QCOMPARE(path, QDir(directory).filePath("한글 #%.bin"));
            QVERIFY(!QFileInfo::exists(path)); // Resolution never creates a file.
            QSaveFile output(path);
            QVERIFY(output.open(QIODevice::WriteOnly));
            QCOMPARE(output.write("shared bytes"), 12);
            QVERIFY(output.commit());
            QFile input(storage->filePath(section, "Client/.state/한글 #%.bin", &error));
            QVERIFY(input.open(QIODevice::ReadOnly));
            QCOMPARE(input.readAll(), QByteArray("shared bytes"));
            QVERIFY(error.isEmpty());
        }
    }

    void nativePathsRejectTraversalAndRedirection()
    {
        QTemporaryDir fixture(QDir::current().filePath("native-boundaries-XXXXXX.societycontainer"));
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto storage = SharedStorage::open(fixture.path());
        QVERIFY(storage);
        const QStringList invalid{"../Files/escape", "/absolute", "a/../../escape", "a//b",
            "a/./b", "a/", "a\\b", "C:alternate", ":/resource", QString("a") + QChar::Null + "b"};
        for (const auto &path : invalid) {
            QString error;
            QVERIFY2(storage->filePath(StoreSection::Models, path, &error).isEmpty(), qPrintable(path));
            QVERIFY(!error.isEmpty());
            QVERIFY(storage->ensureDirectory(StoreSection::Models, path, &error).isEmpty());
        }
        QCOMPARE(QDir(fixture.filePath("Models")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        QVERIFY(QDir(fixture.filePath("Models")).entryList(QDir::Files | QDir::Hidden).isEmpty());
        QVERIFY(storage->filePath(StoreSection::Models, "missing/child").isEmpty());
        QVERIFY(storage->filePath(static_cast<StoreSection>(-1), {}).isEmpty());
        write(fixture.filePath("Models/regular"), "file");
        QVERIFY(storage->filePath(StoreSection::Models, "regular/child").isEmpty());
#ifdef Q_OS_UNIX
        QVERIFY(createTestSymbolicLink(fixture.filePath("Files"), fixture.filePath("Models/redirect")));
        QVERIFY(createTestSymbolicLink(fixture.filePath("not-present"), fixture.filePath("Models/dangling")));
        for (const auto &path : {"redirect", "redirect/escape", "dangling"})
            QVERIFY(storage->filePath(StoreSection::Models, path).isEmpty());
        QCOMPARE(QDir(fixture.filePath("Files")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
#endif
    }

    void pinnedStorageRejectsRootRedirectAndIdentityReplacement()
    {
        QTemporaryDir fixture(QDir::current().filePath("native-identity-XXXXXX"));
        const auto root = fixture.filePath("Society.societycontainer");
        QVERIFY(QDir().mkdir(root));
        QVERIFY(SocietyDrive::create(root));
        const auto storage = SharedStorage::open(root);
        QVERIFY(storage);
#ifdef Q_OS_UNIX
        const auto moved = fixture.filePath("Moved");
        QVERIFY(QDir().rename(root, moved));
        QVERIFY(createTestSymbolicLink(moved, root));
        QVERIFY(!storage->drive().isValid());
        QVERIFY(storage->filePath(StoreSection::Files, "file").isEmpty());
        QVERIFY(QFile::remove(root));
        QVERIFY(QDir().rename(moved, root));
#endif
        QVERIFY(QFile::remove(QDir(root).filePath(".society-drive.json")));
        QVERIFY(SocietyDrive::create(root));
        QVERIFY(storage->filePath(StoreSection::Files, "file").isEmpty());
        QVERIFY(storage->ensureDirectory(StoreSection::Models, "new").isEmpty());
    }

    void appsDiscoverTheSameDriveAndCannotSilentlySwitchIdentity()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-storage-XXXXXX"));
        QVERIFY(QDir().mkdir(fixture.filePath("Society.societycontainer")));
        const auto drive = SocietyDrive::create(fixture.filePath("Society.societycontainer"));
        QVERIFY(drive);
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", fixture.filePath("settings.json").toUtf8());
        qunsetenv("SOCIETY_CONTAINER_PATH");
        QString error;
        QCoreApplication::setApplicationName("Society");
        QVERIFY2(SharedStorage::setDefaultContainer(drive->rootPath(), &error), qPrintable(error));
        QCoreApplication::setApplicationName("Dreamscapes");
        const auto client = SharedStorage::open({}, &error);
        QVERIFY2(client.has_value(), qPrintable(error));
        QCOMPARE(client->drive().identifier(), drive->identifier());
        QCOMPARE(client->drive().sectionPath(StoreSection::Models), fixture.filePath("Society.societycontainer/Models"));
        QVERIFY(QFile::remove(fixture.filePath("Society.societycontainer/.society-drive.json")));
        QVERIFY(SocietyDrive::create(drive->rootPath()));
        QVERIFY(!SharedStorage::open({}, &error));
        QVERIFY(error.contains("identity"));
    }

    void referencesResolveOnlyWithinTheSameModelsArea()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-models-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path());
        QVERIFY(drive);
        write(fixture.filePath("Models/weights.safetensor"), "one");
        write(fixture.filePath("Files/public.safetensors"), "public");
        write(fixture.filePath("Models/.partial.safetensors"), "partial");
        QVERIFY(QDir().mkdir(fixture.filePath("Models/pipeline")));
        write(fixture.filePath("Models/pipeline/model_index.json"), "{\"_class_name\":\"StableDiffusionPipeline\"}");
        write(fixture.filePath("Models/pipeline/model.safetensors"), "weights");
        QVERIFY(QDir().mkpath(fixture.filePath("Models/cascade.iildmodel/members")));
        write(fixture.filePath("Models/cascade.iildmodel/model_index.json"),
              "{\"schema\":\"iild-unified-model-v1\",\"_class_name\":\"IILDUnifiedCascade\"}");
        write(fixture.filePath("Models/cascade.iildmodel/members/model.safetensors"), "member weights");
        const auto client = SharedStorage::open(fixture.path());
        QVERIFY(client);
        const auto models = client->models();
        QCOMPARE(models.size(), 3);
        const auto unified = std::find_if(models.begin(), models.end(), [](const auto &m) { return m.format == "unified"; });
        QVERIFY(unified != models.end());
        QCOMPARE(unified->id, QString("cascade.iildmodel"));
        for (const auto &model : models) {
            auto reference = model.reference(drive->identifier());
            QCOMPARE(client->resolveModel(reference), fixture.filePath("Models/" + model.id));
            reference["containerId"] = "another-drive";
            QVERIFY(client->resolveModel(reference).isEmpty());
        }
        const auto file = *std::find_if(models.begin(), models.end(), [](const auto &m) { return m.format == "safetensors"; });
        auto reference = file.reference(drive->identifier());
        reference["path"] = "../Files/public.safetensors";
        QVERIFY(client->resolveModel(reference).isEmpty());
        write(fixture.filePath("Models/weights.safetensor"), "changed model");
        QVERIFY(client->resolveModel(file.reference(drive->identifier())).isEmpty());
        write(fixture.filePath("Models/cascade.iildmodel/members/model.safetensors"), "changed member");
        QVERIFY(client->resolveModel(unified->reference(drive->identifier())).isEmpty());
    }

    void privateOutputDirectoriesRejectRedirection()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-outputs-XXXXXX.societycontainer"));
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto client = SharedStorage::open(fixture.path());
        QVERIFY(client);
        QCOMPARE(client->filePath(StoreSection::GenerationHistory, "unique-result.png"),
                 fixture.filePath("Generation History/unique-result.png"));
        QVERIFY(QDir(fixture.filePath("Generation History")).isEmpty());
        QCOMPARE(client->ensureDirectory(StoreSection::AssetLibrary, "project"),
                 fixture.filePath("Asset Library/project"));
        QVERIFY(client->ensureDirectory(StoreSection::Models, "../Files/escape").isEmpty());
        QVERIFY(createTestSymbolicLink(fixture.filePath("Files"), fixture.filePath("Asset Library/redirect")));
        QVERIFY(client->ensureDirectory(StoreSection::AssetLibrary, "redirect/job").isEmpty());
        QCOMPARE(QDir(fixture.filePath("Files")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
    }
};

QTEST_GUILESS_MAIN(SharedStorageTests)
#include "shared_storage.moc"
