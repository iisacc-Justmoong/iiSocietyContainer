#include "SymbolicLink.h"
#include <iiFileProviderPresentation.h>
#include <iiFileProviderNative.h>
#include <File.h>
#include <FileCatalog.h>
#include <SharedStorage.h>
#include <StorageMap.h>
#include <StorageDirectoryModel.h>
#include <InventoryIndex.h>
#include <StorageModelCatalog.h>
#include <StorageMaintenance.h>
#include <ModelStore.h>
#include <FileOperations.h>
#include "SignalRecorder.h"
#include "QtEventWait.h"
#include "FlatModelContract.h"
#include <QEvent>
#include <exception>
#include <string>
#include <QJsonArray>
#include <algorithm>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>
#include <QElapsedTimer>
#include <QDataStream>
#include <QScopeGuard>
#include <atomic>
#include <thread>

using namespace iiSocietyContainer;

namespace {
void write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    CTEST_CHECK(file.open(QIODevice::WriteOnly));
    CTEST_COMPARE(file.write(bytes), bytes.size());
}
void makeReadyReplica(const SocietyDrive &drive)
{
    // Keep this fixture's logical namespace stable while creating the actual
    // replica manifest contract, rather than treating missing primary metadata
    // as proof that a newly created source package is a replica.
    QString error;
    CTEST_CHECK_MESSAGE(SocietyDrive::adoptReplicaIdentity(drive.rootPath(), drive.identifier(), drive.identifier(), &error), qPrintable(error));
    CTEST_CHECK_MESSAGE(SocietyDrive::completeReplica(drive.rootPath(), drive.identifier(), &error), qPrintable(error));
}
void removeEmptyModelCategories(const SocietyDrive &drive)
{
    for (const auto type : allModelTypes())
        CTEST_CHECK(QDir().rmdir(drive.rootPath() + "/Models/" + modelTypeName(type)));
}
}

class SharedStorageTests : public QObject
{
public:
    void emptyModelCategoriesAppearInLocalAndReplicaListings(bool replica) {
        QTemporaryDir fixture(QDir::current().filePath("model-empty-categories-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        const auto drive=SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        if (replica) makeReadyReplica(*drive);
        StorageMap map(*drive); CTEST_CHECK(map.publish({}));
        StorageDirectoryModel model; model.setFolder(QUrl::fromLocalFile(fixture.filePath("Models")));
        CTEST_WAIT_COMPARE_TIMEOUT(model.status(),StorageDirectoryModel::Ready,30000);
        CTEST_COMPARE(model.rowCount(),allModelTypes().size());
        QSet<QString> names;
        int checkpoint=-1;
        for (int i=0;i<model.rowCount();++i) {
            CTEST_CHECK(model.get(i,"fileIsDir").toBool());
            const auto name=model.get(i,"fileName").toString(); names.insert(name);
            if (name==QStringLiteral("Checkpoint")) checkpoint=i;
        }
        for (const auto type:allModelTypes()) CTEST_CHECK(names.contains(modelTypeName(type)));
        CTEST_CHECK(checkpoint>=0);
        society_ctest::SignalRecorder opened(&model,&StorageDirectoryModel::activated);
        model.activate(checkpoint);
        CTEST_WAIT_COMPARE_TIMEOUT(opened.size(),1,30000);
        CTEST_COMPARE(opened.first().at(0).toString(),fixture.filePath("Models/Checkpoint"));
        CTEST_CHECK(opened.first().at(1).toBool());
        CTEST_CHECK(map.pendingRequests().isEmpty());
    }
    void legacyClassificationsAreIsolatedWithoutMergingUnknownFiles() {
        QTemporaryDir fixture(QDir::current().filePath("classification-migration-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        const auto storage = SharedStorage::open(fixture.path()); CTEST_CHECK(storage);
        write(fixture.filePath("Files/known.txt"), "known");
        {
            iiFileProvider::FileCatalog old(std::filesystem::path(fixture.filePath("Files").toStdU16String()));
            iiFileProvider::FileMetadata metadata; metadata.title = "Retained title";
            CTEST_CHECK(old.registerFile("known.txt", metadata).metadataWritten);
        }
        write(fixture.filePath("Files/orphan.txt.iisacc.json"), "unrecognized legacy bytes");
        write(fixture.filePath("Files/unindexed.txt"), "unindexed object");
        write(fixture.filePath("Files/unindexed.txt.iisacc.json"),
              "{\"format\":\"iiFileProvider.Metadata\",\"schema\":1,"
              "\"fileKey\":\"file-0123456789abcdef0123456789abcdef\",\"revision\":1,"
              "\"metadata\":{\"title\":\"Unverified title\"},\"collections\":[]}");
        write(fixture.filePath("Files/ordinary.json"), "{\"ordinary\":true}");
        QString error;
        CTEST_CHECK(!storage->reconcileFiles(&error)); // unresolved data must be reported
        CTEST_CHECK(!error.isEmpty());
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("Files/known.txt.iisacc.json")));
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("Files/orphan.txt.iisacc.json")));
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("Files/unindexed.txt.iisacc.json")));
        CTEST_CHECK(QFileInfo::exists(fixture.filePath("Files/ordinary.json")));
        CTEST_CHECK(QFileInfo::exists(fixture.filePath(".society-runtime/classifications/files/known.txt.iisacc.json")));
        const auto info = storage->fileInformation(StoreSection::Files, "known.txt", &error);
        CTEST_COMPARE(info.value("classification").toObject().value("metadata").toObject().value("title").toString(),
                 QStringLiteral("Retained title"));
        const QDir unresolved(fixture.filePath(".society-runtime/classifications/unresolved"));
        bool foundBytes = false, foundAddress = false, foundUnverified = false;
        for (const auto &name : unresolved.entryList(QDir::Files)) {
            QFile saved(unresolved.filePath(name)); CTEST_CHECK(saved.open(QIODevice::ReadOnly));
            const auto content = saved.readAll();
            foundBytes |= content == QByteArray("unrecognized legacy bytes");
            foundAddress |= content.contains("files/orphan.txt.iisacc.json");
            foundUnverified |= content.contains("Unverified title");
        }
        CTEST_CHECK(foundBytes); CTEST_CHECK(foundAddress); CTEST_CHECK(foundUnverified);
        const auto unindexed = storage->fileInformation(StoreSection::Files, "unindexed.txt", &error);
        CTEST_CHECK(unindexed.value("classification").toObject().value("metadata").toObject().value("title").toString()
                != QStringLiteral("Unverified title"));
    }
    void replacedExternalFileDoesNotInheritLegacyClassification() {
        QTemporaryDir fixture(QDir::current().filePath("classification-replaced-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto storage = SharedStorage::open(fixture.path()); CTEST_CHECK(storage);
        const auto asset = fixture.filePath("Files/asset.txt");
        write(asset, "original");
        {
            iiFileProvider::FileCatalog old(std::filesystem::path(fixture.filePath("Files").toStdU16String()));
            iiFileProvider::FileMetadata metadata; metadata.title = "Original title";
            CTEST_CHECK(old.registerFile("asset.txt", metadata).metadataWritten);
        }
        // Keep the old inode alive so the replacement cannot accidentally reuse it.
        CTEST_CHECK(QFile::rename(asset, fixture.filePath("outside-original.txt")));
        write(asset, "replacement");
        QString error;
        CTEST_CHECK(!storage->reconcileFiles(&error));
        CTEST_CHECK(!error.isEmpty());
        CTEST_CHECK(!QFileInfo::exists(asset + ".iisacc.json"));
        const auto info = storage->fileInformation(StoreSection::Files, "asset.txt", &error);
        CTEST_CHECK(info.value("classification").toObject().value("metadata").toObject().value("title").toString()
                != QStringLiteral("Original title"));
        const QDir unresolved(fixture.filePath(".society-runtime/classifications/unresolved"));
        bool savedOriginal = false;
        for (const auto &name : unresolved.entryList(QDir::Files)) {
            QFile saved(unresolved.filePath(name)); CTEST_CHECK(saved.open(QIODevice::ReadOnly));
            savedOriginal |= saved.readAll().contains("Original title");
        }
        CTEST_CHECK(savedOriginal);

        const auto current = fixture.filePath("Files/private.txt");
        write(current, "private original");
        {
            const auto root = std::filesystem::path(fixture.filePath("Files").toStdU16String());
            iiFileProvider::FileCatalog catalog(root, iiFileProvider::FileCatalog::Access::Create,
                FileRepository::classificationDirectory(std::filesystem::path(fixture.path().toStdU16String()), "files"));
            iiFileProvider::FileMetadata metadata; metadata.title = "Private original";
            CTEST_CHECK(catalog.registerFile("private.txt", metadata).metadataWritten);
        }
        CTEST_CHECK(QFile::rename(current, fixture.filePath("outside-private-original.txt")));
        write(current, "private replacement");
        const auto replaced = storage->fileInformation(StoreSection::Files, "private.txt", &error);
        CTEST_COMPARE(replaced.value("classification").toObject().value("status").toString(), QStringLiteral("invalid"));
    }
    void stalePrivateClassificationDoesNotAttachToAReusedPath() {
        QTemporaryDir fixture(QDir::current().filePath("classification-reused-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid()); CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto storage = SharedStorage::open(fixture.path()); CTEST_CHECK(storage);
        const auto oldPath = fixture.filePath("Files/old.txt");
        const auto movedPath = fixture.filePath("Files/moved.txt");
        write(oldPath, "original object");
        {
            const auto root = std::filesystem::path(fixture.filePath("Files").toStdU16String());
            iiFileProvider::FileCatalog catalog(root, iiFileProvider::FileCatalog::Access::Create,
                FileRepository::classificationDirectory(std::filesystem::path(fixture.path().toStdU16String()), "files"));
            iiFileProvider::FileMetadata metadata; metadata.title = "Original private title";
            const auto original = catalog.registerFile("old.txt", metadata); CTEST_CHECK(original.metadataWritten);
            CTEST_CHECK(QFile::rename(oldPath, movedPath));
            const auto moved = catalog.registerFile("moved.txt"); CTEST_CHECK(moved.metadataWritten);
            CTEST_COMPARE(moved.file.key, original.file.key);
        }
        write(oldPath, "another object");
        QString error; CTEST_CHECK(!storage->reconcileFiles(&error)); CTEST_CHECK(!error.isEmpty());
        const auto info = storage->fileInformation(StoreSection::Files, "old.txt", &error);
        CTEST_COMPARE(info.value("classification").toObject().value("status").toString(), QStringLiteral("invalid"));
        const auto movedInfo = storage->fileInformation(StoreSection::Files, "moved.txt", &error);
        CTEST_COMPARE(movedInfo.value("classification").toObject().value("metadata").toObject().value("title").toString(),
                 QStringLiteral("Original private title"));
    }
    void directorySearchAndTypeSortUseCurrentOptions() {
        QTemporaryDir fixture(QDir::current().filePath("directory-search-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        const auto drive=SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        const auto folder=fixture.filePath("Files");
        CTEST_CHECK(QDir().mkdir(QDir(folder).filePath("Alpha folder")));
        write(QDir(folder).filePath("alpha.png"),"image");
        write(QDir(folder).filePath("Beta.json"),"json");
        write(QDir(folder).filePath("Draft*.txt"),"text");
        StorageDirectoryModel model; model.setFolder(QUrl::fromLocalFile(folder));
        CTEST_WAIT_COMPARE_TIMEOUT(model.status(),StorageDirectoryModel::Ready,30000);
        CTEST_COMPARE(model.rowCount(),4);
        CTEST_CHECK(model.setProperty("searchText",QStringLiteral("ALPHA")));
        CTEST_WAIT_COMPARE_TIMEOUT(model.rowCount(),2,30000);
        CTEST_COMPARE(model.get(0,"fileName").toString(),QStringLiteral("Alpha folder"));
        CTEST_COMPARE(model.get(1,"fileName").toString(),QStringLiteral("alpha.png"));
        CTEST_CHECK(model.setProperty("searchText",QStringLiteral("*")));
        CTEST_WAIT_COMPARE_TIMEOUT(model.rowCount(),1,30000);
        CTEST_COMPARE(model.get(0,"fileName").toString(),QStringLiteral("Draft*.txt"));
        CTEST_CHECK(model.setProperty("searchText",QString()));
        CTEST_CHECK(model.setProperty("sortField",int(StorageDirectoryModel::Type)));
        CTEST_WAIT_COMPARE_TIMEOUT(model.rowCount(),4,30000);
        CTEST_WAIT_COMPARE_TIMEOUT(model.get(1,"fileName").toString(),QStringLiteral("Beta.json"),30000);
        CTEST_COMPARE(model.get(2,"fileName").toString(),QStringLiteral("alpha.png"));
        CTEST_COMPARE(model.get(3,"fileName").toString(),QStringLiteral("Draft*.txt"));
    }
    void modelDirectoryHidesAttachedCompanionsInLiveAndCachedRows() {
        QTemporaryDir fixture(QDir::current().filePath("model-tree-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        const auto folder = fixture.filePath("Models/Checkpoint");
        CTEST_CHECK(QDir().mkpath(folder + "/Nested"));
        const QStringList names{"model.safetensors", "model.safetensors.model.json", "model.model.json",
            "model.preview.png", "model.safetensors.preview.jpg", "model.safetensors.iisacc.json",
            "orphan.model.json", "workflow.json"};
        for (const auto &name : names) write(QDir(folder).filePath(name), "fixture");
        const auto expected = QStringList{"Nested", "model.safetensors", "orphan.model.json", "workflow.json"};
        const auto listing = [&](StorageDirectoryModel &model) {
            QStringList result;
            for (int i = 0; i < model.rowCount(); ++i) result.append(model.get(i, "fileName").toString());
            result.sort(); return result;
        };
        const auto url = QUrl::fromLocalFile(folder);
        QByteArray key; QDataStream keyStream(&key, QIODevice::WriteOnly);
        keyStream << url << true << true << false << true << false << false << false
                  << int(StorageDirectoryModel::Name) << QStringList{};
        const auto cacheDir = QDir(StorageDirectoryModel::cacheDirectory(folder)).filePath("directories-v1");
        CTEST_CHECK(QDir().mkpath(cacheDir));
        const auto cache = QDir(cacheDir).filePath(
            QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex()) + ".bin");
        QList<QVariantMap> cachedRows;
        cachedRows.append({{"fileName", "Nested"}, {"filePath", QDir(folder).filePath("Nested")}, {"fileIsDir", true}});
        for (const auto &name : names)
            cachedRows.append({{"fileName", name}, {"filePath", QDir(folder).filePath(name)}, {"fileIsDir", false}});
        QFile cached(cache); CTEST_CHECK(cached.open(QIODevice::WriteOnly));
        QDataStream stream(&cached); stream.setVersion(QDataStream::Qt_6_8);
        stream << quint32(1) << drive->identifier() << cachedRows;
        CTEST_COMPARE(stream.status(), QDataStream::Ok); cached.close();
        StorageDirectoryModel model;
        society_ctest::SignalRecorder restored(&model, &StorageDirectoryModel::snapshotRestored);
        society_ctest::CallbackChecks callbackChecks;
        connect(&model, &StorageDirectoryModel::snapshotRestored, &model, [&] { callbackChecks.capture([&] { CTEST_COMPARE(listing(model), expected); }); });
        model.setFolder(url);
        CTEST_WAIT_COMPARE_TIMEOUT(model.status(), StorageDirectoryModel::Ready, 30000);
        callbackChecks.rethrow();
        CTEST_COMPARE(restored.size(), 1);
        CTEST_COMPARE(listing(model), expected);
        for (const auto &name : names) CTEST_CHECK(QFileInfo::exists(QDir(folder).filePath(name)));
        CTEST_CHECK(model.setProperty("nameFilters", QStringList{"*.json"}));
        CTEST_WAIT_COMPARE_TIMEOUT(listing(model), (QStringList{"Nested", "orphan.model.json", "workflow.json"}), 30000);

        const auto native = std::filesystem::path(fixture.path().toStdU16String());
        auto tree = FileTree(native / "Models").snapshot(); CTEST_CHECK(tree);
        std::vector<InventorySection> sections{{"models", "Models", 1, true, std::move(*tree.value)}};
        CTEST_CHECK(QDir().mkpath(fixture.filePath(".society-runtime/monitor/v1")));
        CTEST_CHECK(InventoryIndex::publish(native, drive->identifier().toStdString(), sections));
        StorageDirectoryModel indexed;
        society_ctest::SignalRecorder indexRestored(&indexed, &StorageDirectoryModel::snapshotRestored);
        connect(&indexed, &StorageDirectoryModel::snapshotRestored, &indexed, [&] { callbackChecks.capture([&] { CTEST_COMPARE(listing(indexed), expected); }); });
        indexed.setFolder(url);
        CTEST_WAIT_COMPARE_TIMEOUT(indexed.status(), StorageDirectoryModel::Ready, 30000);
        callbackChecks.rethrow();
        CTEST_COMPARE(indexRestored.size(), 1);
        CTEST_COMPARE(listing(indexed), expected);

        const auto files = fixture.filePath("Files");
        write(QDir(files).filePath("model.safetensors"), "asset");
        write(QDir(files).filePath("model.model.json"), "ordinary document");
        StorageDirectoryModel ordinary; ordinary.setFolder(QUrl::fromLocalFile(files));
        CTEST_WAIT_COMPARE_TIMEOUT(ordinary.status(), StorageDirectoryModel::Ready, 30000);
        CTEST_COMPARE(ordinary.rowCount(), 2);
    }

    void replicaModelDirectoryHidesCompanionsWithoutDownloading() {
        QTemporaryDir fixture(QDir::current().filePath("model-tree-replica-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        makeReadyReplica(*drive);
        CTEST_CHECK(QDir().mkpath(fixture.filePath("Models/Checkpoint")));
        StorageMap map(*drive);
        QJsonArray objects;
        for (const auto &name : {"remote.safetensors", "remote.model.json", "remote.preview.png", "unrelated.json"})
            objects.append(QJsonObject{{"path", QStringLiteral("models/Checkpoint/") + name},
                {"kind", "file"}, {"size", "7"}, {"resident", false}, {"version", QString(64, 'a')}});
        CTEST_CHECK(map.publish(objects));
        StorageDirectoryModel model;
        model.setFolder(QUrl::fromLocalFile(fixture.filePath("Models/Checkpoint")));
        CTEST_WAIT_COMPARE_TIMEOUT(model.status(), StorageDirectoryModel::Ready, 30000);
        CTEST_COMPARE(model.rowCount(), 2);
        CTEST_COMPARE(model.get(0, "fileName").toString(), QStringLiteral("remote.safetensors"));
        CTEST_COMPARE(model.get(1, "fileName").toString(), QStringLiteral("unrelated.json"));
        CTEST_CHECK(map.pendingRequests().isEmpty());
    }

    void replicaModelTreeNavigatesCatalogDirectoriesWithoutDownloading() {
        QTemporaryDir fixture(QDir::current().filePath("model-virtual-tree-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        removeEmptyModelCategories(*drive); // Simulate an older sparse replica.
        makeReadyReplica(*drive);
        StorageMap map(*drive);
        const QJsonObject selected{{"path", "models/Checkpoint/nested/remote.safetensors"}, {"kind", "file"},
            {"size", "3"}, {"resident", false}, {"version", QString(64, 'a')}};
        auto other = selected; other["path"] = "models/LoRA/other.safetensors";
        const QJsonObject empty{{"path", "models/VAE"}, {"kind", "directory"}};
        CTEST_CHECK(map.publish({selected, other, empty}));
        StorageDirectoryModel root;
        root.setFolder(QUrl::fromLocalFile(fixture.filePath("Models")));
        CTEST_WAIT_COMPARE_TIMEOUT(root.status(), StorageDirectoryModel::Ready, 30000);
        CTEST_COMPARE(root.rowCount(), 3);
        CTEST_COMPARE(root.get(0, "fileName").toString(), QStringLiteral("Checkpoint"));
        CTEST_CHECK(root.get(0, "fileIsDir").toBool());
        const auto checkpoint = fixture.filePath("Models/Checkpoint");
        CTEST_CHECK(!QFileInfo::exists(checkpoint));
        society_ctest::SignalRecorder entered(&root, &StorageDirectoryModel::activated);
        society_ctest::SignalRecorder failed(&root, &StorageDirectoryModel::downloadFailed);
        root.activate(0);
        CTEST_WAIT_COMPARE_TIMEOUT(entered.size(), 1, 30000);
        CTEST_COMPARE(entered.first().at(0).toString(), checkpoint);
        CTEST_CHECK(entered.first().at(1).toBool());
        CTEST_CHECK(failed.isEmpty()); CTEST_CHECK(QFileInfo(checkpoint).isDir());
        CTEST_CHECK(map.pendingRequests().isEmpty());
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("Models/LoRA")));

        StorageDirectoryModel nested;
        nested.setFolder(QUrl::fromLocalFile(checkpoint));
        CTEST_WAIT_COMPARE_TIMEOUT(nested.status(), StorageDirectoryModel::Ready, 30000);
        CTEST_COMPARE(nested.rowCount(), 1);
        CTEST_COMPARE(nested.get(0, "fileName").toString(), QStringLiteral("nested"));
        society_ctest::SignalRecorder nestedEntered(&nested, &StorageDirectoryModel::activated);
        nested.activate(0);
        CTEST_WAIT_COMPARE_TIMEOUT(nestedEntered.size(), 1, 30000);
        const auto folder = fixture.filePath("Models/Checkpoint/nested");
        CTEST_CHECK(QFileInfo(folder).isDir());
        StorageDirectoryModel files; files.setFolder(QUrl::fromLocalFile(folder));
        CTEST_WAIT_COMPARE_TIMEOUT(files.status(), StorageDirectoryModel::Ready, 30000);
        CTEST_COMPARE(files.rowCount(), 1);
        CTEST_COMPARE(files.get(0, "fileName").toString(), QStringLiteral("remote.safetensors"));
        CTEST_CHECK(!files.get(0, "fileResident").toBool());
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("Models/Checkpoint/nested/remote.safetensors")));
        CTEST_CHECK(map.pendingRequests().isEmpty());
        files.activate(0);
        CTEST_WAIT_COMPARE_TIMEOUT(map.pendingRequests().size(), 1, 30000);
        const auto requested = map.pendingRequests().first().toObject().value("objects").toArray();
        CTEST_COMPARE(requested.size(), 1);
        CTEST_COMPARE(requested.first().toObject().value("path"), selected.value("path"));
    }

    void replicaModelTreeRejectsDeletedAndRedirectedFolders() {
        QTemporaryDir fixture(QDir::current().filePath("model-virtual-safety-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        removeEmptyModelCategories(*drive);
        makeReadyReplica(*drive);
        StorageMap map(*drive);
        const QJsonObject deleted{{"path", "models/Checkpoint"}, {"kind", "deleted"}};
        const QJsonObject stale{{"path", "models/Checkpoint/old.safetensors"}, {"kind", "file"},
            {"size", "3"}, {"resident", false}, {"version", QString(64, 'a')}};
        auto live = stale; live["path"] = "models/LoRA/live.safetensors";
        CTEST_CHECK(map.publish({deleted, stale, live}));
        StorageDirectoryModel model; model.setFolder(QUrl::fromLocalFile(fixture.filePath("Models")));
        CTEST_WAIT_COMPARE_TIMEOUT(model.status(), StorageDirectoryModel::Ready, 30000);
        CTEST_COMPARE(model.rowCount(), 1);
        CTEST_COMPARE(model.get(0, "fileName").toString(), QStringLiteral("LoRA"));
        const auto outside = fixture.filePath("outside"); CTEST_CHECK(QDir().mkdir(outside));
        const auto redirect = fixture.filePath("Models/LoRA");
        if (!createTestSymbolicLink(outside, redirect)) society_ctest::skip("Symbolic links are unavailable on this host");
        society_ctest::SignalRecorder opened(&model, &StorageDirectoryModel::activated);
        society_ctest::SignalRecorder failed(&model, &StorageDirectoryModel::downloadFailed);
        model.activate(0);
        CTEST_WAIT_COMPARE_TIMEOUT(failed.size(), 1, 30000);
        CTEST_CHECK(opened.isEmpty());
        CTEST_CHECK(QDir(outside).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
        CTEST_CHECK(map.pendingRequests().isEmpty());
        CTEST_CHECK(removeTestSymbolicLink(redirect));
    }
    void replicaModelTreeRechecksDeletedDescendantsBeforeOpening() {
        QTemporaryDir fixture(QDir::current().filePath("model-virtual-stale-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        removeEmptyModelCategories(*drive);
        makeReadyReplica(*drive);
        StorageMap map(*drive);
        const QJsonObject oldFile{{"path", "models/Checkpoint/old/model.safetensors"}, {"kind", "file"},
            {"size", "3"}, {"resident", false}, {"version", QString(64, 'a')}};
        CTEST_CHECK(map.publish({oldFile}));
        StorageDirectoryModel model; model.setFolder(QUrl::fromLocalFile(fixture.filePath("Models")));
        CTEST_WAIT_COMPARE_TIMEOUT(model.status(), StorageDirectoryModel::Ready, 30000);
        CTEST_COMPARE(model.rowCount(), 1);
        const QJsonObject deleted{{"path", "models/Checkpoint/old"}, {"kind", "deleted"}};
        CTEST_CHECK(map.publish({deleted, oldFile}));
        society_ctest::SignalRecorder opened(&model, &StorageDirectoryModel::activated);
        society_ctest::SignalRecorder failed(&model, &StorageDirectoryModel::downloadFailed);
        model.activate(0);
        CTEST_WAIT_COMPARE_TIMEOUT(failed.size(), 1, 30000);
        CTEST_CHECK(opened.isEmpty());
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("Models/Checkpoint")));
        CTEST_CHECK(map.pendingRequests().isEmpty());
    }

    void staleDirectorySnapshotNeverExposesProviderMetadata() {
        QTemporaryDir fixture(QDir::current().filePath("old-metadata-cache-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        const auto files = fixture.filePath("Files");
        const auto ordinary = QDir(files).filePath("document.json");
        const auto metadata = QDir(files).filePath("document.json.iisacc.json");
        write(ordinary, "{}"); write(metadata, "old metadata");
        const auto folder = QUrl::fromLocalFile(files);
        QByteArray key; QDataStream keyStream(&key, QIODevice::WriteOnly);
        keyStream << folder << true << true << false << true << false << false << false
                  << int(StorageDirectoryModel::Name) << QStringList{};
        const auto directory = QDir(StorageDirectoryModel::cacheDirectory(files)).filePath("directories-v1");
        CTEST_CHECK(QDir().mkpath(directory));
        const auto cache = QDir(directory).filePath(
            QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex()) + ".bin");
        const QList<QVariantMap> rows{
            {{"fileName", "document.json"}, {"filePath", ordinary}, {"fileIsDir", false}},
            {{"fileName", "document.json.iisacc.json"}, {"filePath", metadata}, {"fileIsDir", false}},
            {{"fileName", "misleading.txt"}, {"filePath", metadata}, {"fileIsDir", false}}};
        QFile cached(cache); CTEST_CHECK(cached.open(QIODevice::WriteOnly));
        QDataStream stream(&cached); stream.setVersion(QDataStream::Qt_6_8);
        stream << quint32(1) << drive->identifier() << rows;
        CTEST_COMPARE(stream.status(), QDataStream::Ok); cached.close();
        StorageDirectoryModel model;
        society_ctest::SignalRecorder restored(&model, &StorageDirectoryModel::snapshotRestored);
        society_ctest::CallbackChecks callbackChecks;
        connect(&model, &StorageDirectoryModel::snapshotRestored, &model, [&] { callbackChecks.capture([&] {
            CTEST_COMPARE(model.rowCount(), 1);
            CTEST_COMPARE(model.get(0, "fileName").toString(), QStringLiteral("document.json"));
        }); });
        model.setFolder(folder);
        CTEST_WAIT_COMPARE_TIMEOUT(model.status(), StorageDirectoryModel::Ready, 30000);
        callbackChecks.rethrow();
        CTEST_COMPARE(restored.size(), 1);
        CTEST_COMPARE(model.rowCount(), 1);
        CTEST_COMPARE(model.get(0, "fileName").toString(), QStringLiteral("document.json"));
        CTEST_CHECK(QFileInfo::exists(metadata));
        society_ctest::SignalRecorder opened(&model, &StorageDirectoryModel::activated); society_ctest::SignalRecorder failed(&model, &StorageDirectoryModel::downloadFailed);
        model.openPath(metadata);
        CTEST_WAIT_COMPARE_TIMEOUT(failed.size(), 1, 30000);
        CTEST_CHECK(opened.isEmpty());
    }
    void daemonInventoryRestoresBeforeLiveValidation() {
        QTemporaryDir fixture(QDir::current().filePath("daemon-index-XXXXXX.societycontainer")); CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        const auto files = fixture.filePath("Files");
        write(QDir(files).filePath("old.txt"), "cached");
        write(QDir(files).filePath("replace.txt"), "before");
        write(QDir(files).filePath("stable.txt"), "available");
        CTEST_CHECK(QDir().mkpath(fixture.filePath(".society-runtime/monitor/v1")));
        const auto native = std::filesystem::path(fixture.path().toStdU16String());
        auto tree = FileTree(native / "Files").snapshot(); CTEST_CHECK(tree);
        std::vector<InventorySection> sections{{"files", "Files", 1, true, std::move(*tree.value)}};
        const auto saved = InventoryIndex::publish(native, drive->identifier().toStdString(), sections);
        CTEST_CHECK_MESSAGE(saved.has_value(), saved ? "" : saved.error().c_str());
        CTEST_CHECK(QFile::remove(QDir(files).filePath("old.txt")));
        write(QDir(files).filePath("replace.txt"), "changed contents");
        write(QDir(files).filePath("new.txt"), "current");
        StorageDirectoryModel model;
        society_ctest::SignalRecorder restored(&model, &StorageDirectoryModel::snapshotRestored);
        society_ctest::SignalRecorder opened(&model, &StorageDirectoryModel::activated);
        society_ctest::SignalRecorder failed(&model, &StorageDirectoryModel::downloadFailed);
        society_ctest::CallbackChecks callbackChecks;
        connect(&model, &StorageDirectoryModel::snapshotRestored, &model, [&] { callbackChecks.capture([&] {
            CTEST_COMPARE(model.status(), StorageDirectoryModel::Loading);
            CTEST_COMPARE(model.rowCount(), 3);
            CTEST_COMPARE(model.get(0, "fileName").toString(), "old.txt");
            model.activate(0); CTEST_CHECK(failed.wait(15000)); CTEST_COMPARE(opened.size(), 0);
            CTEST_COMPARE(model.get(1, "fileName").toString(), "replace.txt");
            model.activate(1); CTEST_CHECK(failed.wait(15000)); CTEST_COMPARE(opened.size(), 0);
            CTEST_COMPARE(model.get(2, "fileName").toString(), "stable.txt");
            model.activate(2); CTEST_CHECK(opened.wait(15000));
            CTEST_COMPARE(opened.at(0).at(0).toString(), QDir(files).filePath("stable.txt"));
            CTEST_COMPARE(model.status(), StorageDirectoryModel::Loading);
        }); });
        model.setFolder(QUrl::fromLocalFile(files));
        CTEST_WAIT_COMPARE_TIMEOUT(model.status(), StorageDirectoryModel::Ready, 15000);
        callbackChecks.rethrow();
        CTEST_COMPARE(restored.size(), 1);
        CTEST_COMPARE(model.rowCount(), 3);
        CTEST_COMPARE(model.get(0, "fileName").toString(), "new.txt");
    }
    void modelInventoryExcludesOnnxAndKeepsGguf() {
        QTemporaryDir fixture(QDir::current().filePath("model-extension-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        const auto storage = SharedStorage::open(fixture.path()); CTEST_CHECK(storage);
        const auto base = std::filesystem::path(fixture.filePath("Models").toStdU16String());
        const std::string header = R"({"weight":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})";
        std::string checkpoint(8, '\0');
        for (unsigned i = 0; i < 8; ++i) checkpoint[i] = static_cast<char>((header.size() >> (8 * i)) & 255);
        checkpoint += header; checkpoint.append(4, '\0');
        iiFileProvider::native::File::create(base / "model.safetensors", checkpoint);
        iiFileProvider::native::File::create(base / "legacy.safetensor", checkpoint);
        iiFileProvider::native::File::create(base / "legacy.onnx", "untouched ONNX bytes");
        iiFileProvider::native::File::create(base / "legacy.gguf", "untouched GGUF bytes");
        auto models = storage->models();
        CTEST_COMPARE(models.size(), 3);
        const auto gguf = std::find_if(models.begin(), models.end(), [](const auto &m) { return m.id == "legacy.gguf"; });
        CTEST_CHECK(gguf != models.end()); CTEST_COMPARE(gguf->format, "gguf");
        const QFileInfo legacy(fixture.filePath("Models/legacy.safetensor"));
        QCryptographicHash fingerprint(QCryptographicHash::Sha256);
        fingerprint.addData(QByteArray("legacy.safetensor"));
        fingerprint.addData(QByteArray::number(legacy.size()));
        fingerprint.addData(QByteArray::number(legacy.lastModified().toMSecsSinceEpoch()));
        fingerprint.addData(QByteArray(checkpoint.data(), static_cast<qsizetype>(checkpoint.size())));
        const QJsonObject oldReference{{"containerId", drive->identifier()}, {"path", "legacy.safetensor"},
            {"format", "safetensors"}, {"fingerprint", QString::fromLatin1(fingerprint.result().toHex())}};
        CTEST_COMPARE(storage->resolveModel(oldReference), legacy.absoluteFilePath());

        StorageMap map(*drive);
        QJsonArray entries;
        for (const auto &name : {QStringLiteral("model.safetensors"), QStringLiteral("legacy.safetensor"),
                                 QStringLiteral("legacy.onnx"), QStringLiteral("legacy.gguf")}) {
            entries.append(QJsonObject{{"path", "models/" + name}, {"kind", "file"},
                {"size", QString::number(QFileInfo(fixture.filePath("Models/" + name)).size())},
                {"resident", true}, {"version", QString(64, 'a')}});
        }
        CTEST_CHECK(map.publish(entries));
        models = storage->models();
        CTEST_COMPARE(models.size(), 3);
        const auto syncedGguf = std::find_if(models.begin(), models.end(), [](const auto &m) { return m.id == "legacy.gguf"; });
        CTEST_CHECK(syncedGguf != models.end()); CTEST_COMPARE(syncedGguf->format, "gguf");
        CTEST_CHECK(std::none_of(models.begin(), models.end(), [](const auto &m) { return m.id.endsWith(".onnx", Qt::CaseInsensitive); }));
        CTEST_COMPARE(iiFileProvider::native::File::read(base / "legacy.safetensor"), checkpoint);
        CTEST_COMPARE(iiFileProvider::native::File::read(base / "legacy.onnx"), std::string("untouched ONNX bytes"));
        CTEST_COMPARE(iiFileProvider::native::File::read(base / "legacy.gguf"), std::string("untouched GGUF bytes"));
    }
    void fileInformationExposesProviderPresentationAndClassification() {
        QTemporaryDir fixture(QDir::current().filePath("rich-information-XXXXXX.societycontainer")); CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto storage = SharedStorage::open(fixture.path()); CTEST_CHECK(storage);
        const auto root = std::filesystem::path(fixture.filePath("Files").toStdU16String());
        iiFileProvider::FileCatalog catalog(root);
        iiFileProvider::FileMetadata annotation; annotation.title = "Campaign document"; annotation.description = "Approved copy";
        annotation.keywords = {"campaign", "approved"}; annotation.rating = 5; annotation.color = iiFileProvider::ColorLabel::Blue;
        annotation.fields = {{"project", "launch"}};
        annotation.collections = {catalog.createCollection("Launch assets").key};
        const std::string body = "Original searchable document";
        const auto created = catalog.createFile("Brief #한글.txt", {reinterpret_cast<const std::byte *>(body.data()), body.size()}, annotation);
        CTEST_CHECK(created.metadataWritten);
        QString error;
        const auto information = storage->fileInformation(StoreSection::Files, "Brief #한글.txt", &error);
        CTEST_CHECK_MESSAGE(!information.isEmpty(), qPrintable(error));
        CTEST_COMPARE(information.value("format").toString(), "iiFileProvider.FileInformation");
        CTEST_COMPARE(information.value("path").toString(), "files/Brief #한글.txt");
        CTEST_COMPARE(information.value("details").toObject().value("mimeType").toString(), "text/plain");
        CTEST_COMPARE(information.value("preview").toObject().value("kind").toString(), "text");
        CTEST_CHECK(!information.value("preview").toObject().value("data").toString().isEmpty());
        const auto classification = information.value("classification").toObject();
        CTEST_COMPARE(classification.value("status").toString(), "ready");
        CTEST_COMPARE(classification.value("metadata").toObject().value("rating").toInt(), 5);
        CTEST_COMPARE(classification.value("metadata").toObject().value("color").toString(), "blue");
        CTEST_COMPARE(classification.value("collections").toArray().first().toObject().value("name").toString(), "Launch assets");
        CTEST_CHECK(!information.contains("rootPath"));
        StorageDirectoryModel listing; listing.setFolder(QUrl::fromLocalFile(fixture.filePath("Files")));
        CTEST_WAIT_COMPARE(listing.status(), StorageDirectoryModel::Ready);
        CTEST_COMPARE(listing.rowCount(), 1);
        CTEST_COMPARE(listing.get(0, "fileInformationProvider").toString(), "iiFileProvider");
        CTEST_COMPARE(listing.get(0, "fileInformationKey").toString(), "files/Brief #한글.txt");
        write(fixture.filePath("Files/external.bin"), QByteArray("\0binary", 7));
        const auto external = storage->fileInformation(StoreSection::Files, "external.bin", &error);
        CTEST_CHECK_MESSAGE(!external.isEmpty(), qPrintable(error));
        CTEST_COMPARE(external.value("classification").toObject().value("status").toString(), "absent");
        write(fixture.filePath("Files/external.bin.iisacc.json"), "{\"rating\":99}");
        const auto malformed = storage->fileInformation(StoreSection::Files, "external.bin", &error);
        CTEST_COMPARE(malformed.value("classification").toObject().value("status").toString(), "invalid");
        CTEST_CHECK(!malformed.value("preview").toObject().value("data").toString().isEmpty());
        CTEST_CHECK(storage->fileInformation(StoreSection::Files, "Brief #한글.txt.iisacc.json", &error).isEmpty());
        CTEST_CHECK(storage->fileInformation(StoreSection::Files, "../Models/a", &error).isEmpty());
        CTEST_CHECK(storage->fileInformation(StoreSection::Files, "", &error).isEmpty());
        CTEST_COMPARE(iiFileProvider::native::File::read(root / std::filesystem::path(u8"Brief #한글.txt")), body);
        iiFileProvider::native::Presentation::cancelPending(std::filesystem::path(fixture.path().toStdU16String()));
    }
    void externalFilesReceiveProviderDetailsAndPreviewWithoutAnAppSave() {
        QTemporaryDir fixture(QDir::current().filePath("external-preview-XXXXXX.societycontainer")); CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        const auto registeredRoot = std::filesystem::path(drive->rootPath().toStdU16String());
        const auto maintenance = StorageMaintenance::runOnce(registeredRoot, drive->identifier().toStdString());
        CTEST_CHECK_MESSAGE(maintenance.errors.empty(), maintenance.errors.empty() ? "" : maintenance.errors.front().c_str());
        const auto path = fixture.filePath("Files/External #한글.txt");
        const QByteArray contents = "A file copied by an external application";
        write(path, contents); // Intentional foreign writer fixture.
        const auto native = std::filesystem::path(path.toStdU16String());
        CTEST_WAIT_VERIFY(iiFileProvider::native::Presentation::load(native).has_value(), 15000);
        const auto record = iiFileProvider::native::Presentation::load(native); CTEST_CHECK(record);
        CTEST_COMPARE(record->preview.kind, std::string("text"));
        CTEST_COMPARE(record->details.size, std::uint64_t(contents.size()));
        QFile original(path); CTEST_CHECK(original.open(QIODevice::ReadOnly)); CTEST_COMPARE(original.readAll(), QByteArray("A file copied by an external application"));
        iiFileProvider::native::Presentation::cancelPending(std::filesystem::path(fixture.path().toStdU16String()));
    }
    void generatedPngPublicationPreservesBytesAndRejectsCollisionsAndRedirects() {
        QTemporaryDir fixture(QDir::current().filePath("publish-history-XXXXXX.societycontainer")); CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto storage = SharedStorage::open(fixture.path()); CTEST_CHECK(storage);
        const auto bytes = QByteArray::fromHex("89504e470d0a1a0a") + "original PNG payload";
        const auto source = fixture.filePath("Files/source.png"); write(source, bytes);
        QString error;
        const auto path = storage->publishGeneratedImage(source, "한글 #%-0001.PNG", &error);
        CTEST_COMPARE(path, fixture.filePath("Generation History/한글 #%-0001.PNG")); CTEST_CHECK(error.isEmpty());
        QFile output(path); CTEST_CHECK(output.open(QIODevice::ReadOnly)); CTEST_COMPARE(output.readAll(), bytes); output.close();
        CTEST_CHECK(QFileInfo::exists(source));
        CTEST_CHECK(storage->publishGeneratedImage(source, "한글 #%-0001.PNG", &error).isEmpty()); CTEST_CHECK(!error.isEmpty());
        CTEST_CHECK(output.open(QIODevice::ReadOnly)); CTEST_COMPARE(output.readAll(), bytes); output.close();
        for (const auto &name : {"result.iisc", "../Files/result.png", "app/result.png", ".hidden.png", "result\\name.png"}) {
            CTEST_CHECK(storage->publishGeneratedImage(source, name, &error).isEmpty()); CTEST_CHECK(!error.isEmpty());
        }
        write(fixture.filePath("Files/invalid.png"), "native canvas or invalid PNG");
        CTEST_CHECK(storage->publishGeneratedImage(fixture.filePath("Files/invalid.png"), "invalid.png", &error).isEmpty());
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("Generation History/invalid.png")));
        const auto link = fixture.filePath("Files/link.png"); CTEST_CHECK(createTestSymbolicLink(source, link));
        CTEST_CHECK(storage->publishGeneratedImage(link, "link.png", &error).isEmpty());
        const QDir history(fixture.filePath("Generation History"));
        CTEST_CHECK(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(15)));
        auto published = history.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        published.removeIf([](const QString& name) {
            return iiFileProvider::native::Presentation::isControlFile(std::filesystem::path(name.toStdU16String()));
        });
        CTEST_COMPARE(published, QStringList{"한글 #%-0001.PNG"});
        CTEST_CHECK(iiFileProvider::native::Presentation::load(std::filesystem::path(path.toStdU16String())));
        iiFileProvider::native::Presentation::cancelPending(std::filesystem::path(fixture.path().toStdU16String()));
        CTEST_CHECK(iiFileProvider::File::remove(path));
        const auto lease = history.filePath(".iisacc-temporary.lock");
        if (QFileInfo::exists(lease)) { CTEST_COMPARE(QFileInfo(lease).size(), 0); CTEST_CHECK(QFile::remove(lease)); }
        // A published file now owns a durable classification index. Preserve
        // that fixture state while replacing the public section with a link.
        CTEST_CHECK(QDir().rename(history.path(), fixture.filePath(".retained-history")));
        CTEST_CHECK(createTestSymbolicLink(fixture.filePath("Files"), history.path()));
        CTEST_CHECK(storage->publishGeneratedImage(source, "escape.png", &error).isEmpty());
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("Files/escape.png")));
    }
    void runtimePathsStayInsideTheSelectedPackageAndCreateOnlyOnRequest()
    {
        QTemporaryDir fixture(QDir::current().filePath("runtime-boundary-XXXXXX.societycontainer")); CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        const auto storage = SharedStorage::open(fixture.path()); CTEST_CHECK(storage);
        const auto runtime = fixture.filePath(".society-runtime");
        CTEST_COMPARE(storage->runtimePath(), runtime);
        CTEST_COMPARE(storage->runtimePath("state/helpers/session.json"), runtime + "/state/helpers/session.json");
        CTEST_CHECK(!QFileInfo::exists(runtime));
        for (const auto &path : {"../Files", "a/../../Files", "a//b", "/absolute", "a\\b", "a:stream", ".", "a/"}) {
            CTEST_CHECK(storage->runtimePath(path).isEmpty());
            CTEST_CHECK(storage->ensureRuntimeDirectory(path).isEmpty());
        }
        CTEST_CHECK(!QFileInfo::exists(runtime));
        CTEST_COMPARE(storage->ensureRuntimeDirectory("state/helpers"), runtime + "/state/helpers");
        CTEST_CHECK(!QFileInfo::exists(runtime + "/cache"));
        write(runtime + "/state/regular", "user bytes");
        CTEST_CHECK(storage->runtimePath("state/regular/child").isEmpty());
        CTEST_CHECK(storage->ensureRuntimeDirectory("state/regular").isEmpty());
        CTEST_CHECK(createTestSymbolicLink(fixture.filePath("Files"), runtime + "/redirect"));
        CTEST_CHECK(storage->runtimePath("redirect/escape").isEmpty());
        CTEST_CHECK(storage->ensureRuntimeDirectory("redirect/escape").isEmpty());
        CTEST_CHECK(QDir(fixture.filePath("Files")).isEmpty());
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto replica = SocietyDrive::adoptReplicaIdentity(fixture.path(), drive->identifier(), host); CTEST_CHECK(replica);
        CTEST_CHECK(storage->runtimePath("state").isEmpty());
        const auto owner = SharedStorage::open(fixture.path(), nullptr, true); CTEST_CHECK(owner);
        CTEST_CHECK(!owner->drive().isReady());
        CTEST_COMPARE(owner->ensureRuntimeDirectory("sync"), runtime + "/sync");
        CTEST_CHECK(!SharedStorage::open(fixture.path()));
    }
    void cachedDirectorySnapshotIsReconciledBeforeOpening() {
        QTemporaryDir fixture(QDir::current().filePath("directory-cache-XXXXXX.societycontainer")); CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto files = fixture.filePath("Files");
        write(QDir(files).filePath("old.txt"), "original");
        {
            StorageDirectoryModel initial; initial.setFolder(QUrl::fromLocalFile(files));
            CTEST_WAIT_COMPARE(initial.status(), StorageDirectoryModel::Ready); CTEST_COMPARE(initial.rowCount(), 1);
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
            CTEST_WAIT_DEFAULT(persisted());
        }
        CTEST_CHECK(QFile::remove(QDir(files).filePath("old.txt"))); write(QDir(files).filePath("new.txt"), "current");
        StorageDirectoryModel restored;
        society_ctest::SignalRecorder cached(&restored, &StorageDirectoryModel::snapshotRestored);
        society_ctest::SignalRecorder activated(&restored, &StorageDirectoryModel::activated);
        society_ctest::CallbackChecks callbackChecks;
        connect(&restored, &StorageDirectoryModel::snapshotRestored, &restored, [&] { callbackChecks.capture([&] {
            CTEST_COMPARE(restored.status(), StorageDirectoryModel::Loading);
            CTEST_COMPARE(restored.get(0, "fileName").toString(), "old.txt");
            restored.activate(0); CTEST_COMPARE(activated.size(), 0);
        }); });
        restored.setFolder(QUrl::fromLocalFile(files));
        CTEST_WAIT_COMPARE(restored.status(), StorageDirectoryModel::Ready);
        callbackChecks.rethrow();
        CTEST_COMPARE(cached.size(), 1); CTEST_COMPARE(restored.rowCount(), 1);
        CTEST_COMPARE(restored.get(0, "fileName").toString(), "new.txt");
        CTEST_COMPARE(restored.entry(0).value("filePath"), restored.get(0, "filePath"));
        CTEST_CHECK(restored.entry(-1).isEmpty());
        CTEST_CHECK(restored.entry(restored.rowCount()).isEmpty());
        write(QDir(files).filePath("watched.txt"), "observed");
        CTEST_WAIT_COMPARE_TIMEOUT(restored.rowCount(), 2, 5000); // Still before the ten-second fallback, without explicit refresh.
    }
    void generationInventoryReconcilesAuthorityAndPreservesReplicas() {
        QTemporaryDir fixture(QDir::current().filePath("generation-inventory-XXXXXX.societycontainer")); CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        StorageMap map(*drive);
        const QJsonObject old{{"path", "models/Checkpoint/old.safetensors"}, {"kind", "file"},
            {"size", "7"}, {"resident", true}, {"version", QString(64, 'a')}};
        CTEST_CHECK(QDir().mkpath(fixture.filePath("Models/Checkpoint")));
        write(fixture.filePath("Models/Checkpoint/old.safetensors"), "fixture");
        CTEST_CHECK(map.publish({old}));
        auto storage = SharedStorage::open(fixture.path()); CTEST_CHECK(storage);
        const auto reference = storage->models().first().reference(drive->identifier());
        write(fixture.filePath(".society-sync/primary.json"), QJsonDocument(QJsonObject{
            {"schema", 1}, {"container", drive->identifier()}, {"scope", QString(64, 'a')}, {"host", "test-host"}}).toJson());
        CTEST_COMPARE(storage->models().first().reference(drive->identifier()), reference);
        CTEST_CHECK(FileOperations(*drive).perform(FileOperations::Action::Trash,
            fixture.filePath("Models/Checkpoint/old.safetensors")));
        write(fixture.filePath("Models/Checkpoint/new.safetensors"), "new weights");
        CTEST_CHECK(QDir().mkpath(fixture.filePath("Models/Checkpoint/package")));
        write(fixture.filePath("Models/Checkpoint/package/model_index.json"), "{}");
        write(fixture.filePath("Models/Checkpoint/package/model.safetensors"), "package weights");
        CTEST_CHECK(QDir().mkpath(fixture.filePath("Models/.society-runtime")));
        write(fixture.filePath("Models/.society-runtime/hidden.safetensors"), "hidden");
        CTEST_CHECK(createTestSymbolicLink(fixture.filePath("Deleted/old.safetensors"), fixture.filePath("Models/Checkpoint/link.safetensors")));
        const auto models = storage->models(); CTEST_COMPARE(models.size(), 2);
        CTEST_COMPARE(models[0].id, "Checkpoint/new.safetensors"); CTEST_CHECK(models[0].available);
        CTEST_COMPARE(models[0].bytes, 11);
        CTEST_COMPARE(models[1].id, "Checkpoint/package"); CTEST_COMPARE(models[1].format, "diffusers");
        CTEST_CHECK(storage->resolveModel(reference).isEmpty());
        for (const auto &model : models)
            CTEST_COMPARE(storage->resolveModel(model.reference(drive->identifier())), fixture.filePath("Models/" + model.id));
        CTEST_COMPARE(map.objects(), QJsonArray({old})); CTEST_CHECK(map.pendingRequests().isEmpty());
        // Replicas retain remote, nonresident models and never resurrect local leftovers.
        CTEST_CHECK(QFile::remove(fixture.filePath(".society-sync/primary.json")));
        makeReadyReplica(*drive);
        const auto remote = storage->models(); CTEST_COMPARE(remote.size(), 1);
        CTEST_COMPARE(remote.first().id, "Checkpoint/old.safetensors"); CTEST_CHECK(!remote.first().available);
        CTEST_CHECK(map.publish({})); CTEST_CHECK(storage->models().isEmpty());
    }

    void originalModelFilesRemainVisibleWithAPartialCatalogAndNoRegisteredHost() {
        QTemporaryDir fixture(QDir::current().filePath("original-model-catalog-XXXXXX.societycontainer")); CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        StorageMap map(*drive);
        const QJsonArray partial{
            QJsonObject{{"path", "models/Checkpoint"}, {"kind", "directory"}},
            QJsonObject{{"path", "models/LoRA"}, {"kind", "directory"}}};
        CTEST_CHECK(map.publish(partial));
        QStringList paths;
        for (const auto &category : {QString("Checkpoint"), QString("LoRA")}) {
            CTEST_CHECK(QDir().mkpath(fixture.filePath("Models/" + category)));
            const int count = category == "Checkpoint" ? 2 : 6;
            for (int i = 0; i < count; ++i) {
                const auto path = fixture.filePath(QString("Models/%1/user-%2.safetensors").arg(category).arg(i));
                write(path, "user weights"); paths.append(path);
            }
        }
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath(".society-sync/primary.json")));
        CTEST_CHECK(map.isLocalAuthority());
        StorageModelCatalog catalog; catalog.setDirectory(fixture.filePath("Models"));
        CTEST_WAIT_COMPARE(catalog.count(), 8);
        CTEST_COMPARE(catalog.groups().value("Checkpoint").toList().size(), 2);
        CTEST_COMPARE(catalog.groups().value("LoRA").toList().size(), 6);
        const auto storage = SharedStorage::open(fixture.path()); CTEST_CHECK(storage);
        const auto inventory = storage->models(); CTEST_COMPARE(inventory.size(), 8);
        for (const auto &model : inventory) {
            CTEST_CHECK(model.available);
            QString error;
            const auto resolved = storage->resolveModel(model.reference(drive->identifier()), &error);
            CTEST_CHECK_MESSAGE(paths.contains(resolved), qPrintable(error));
        }
        CTEST_COMPARE(map.objects(), partial); CTEST_CHECK(map.pendingRequests().isEmpty());
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath(".society-sync/primary.json")));
        for (const auto &path : paths) {
            QFile source(path); CTEST_CHECK(source.open(QIODevice::ReadOnly));
            CTEST_COMPARE(source.readAll(), QByteArray("user weights"));
        }
    }

    void localModelReferenceRemainsStableWhileProviderPublishesPreviews() {
        QTemporaryDir fixture(QDir::current().filePath("model-preview-reference-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        StorageMap map(*drive); CTEST_CHECK(map.publish({}));
        CTEST_CHECK(QDir().mkpath(fixture.filePath("Models/Checkpoint")));
        const auto path = fixture.filePath("Models/Checkpoint/stable.safetensors");
        write(path, "user model weights");
        const auto storage = SharedStorage::open(fixture.path()); CTEST_CHECK(storage);
        const auto models = storage->models(); CTEST_COMPARE(models.size(), 1);
        const auto reference = models.first().reference(drive->identifier());
        const auto nativePath = std::filesystem::path(path.toStdU16String());
        const auto bitmapPath = std::filesystem::path(fixture.filePath("preview.bmp").toStdU16String());
        std::string bitmap(66, '\0'); bitmap[0] = 'B'; bitmap[1] = 'M';
        const auto put32 = [&](std::size_t at, std::uint32_t value) {
            for (int i = 0; i < 4; ++i) bitmap[at + std::size_t(i)] = char(value >> (i * 8));
        };
        put32(2, 66); put32(10, 54); put32(14, 40); put32(18, 3); put32(22, 1);
        bitmap[26] = 1; bitmap[28] = 24; put32(34, 12); bitmap[56] = char(255);
        iiFileProvider::native::File::write(bitmapPath, bitmap);
        const auto preview = iiFileProvider::native::Presentation::prepare(bitmapPath).preview;
        CTEST_COMPARE(preview.mimeType, std::string("image/png"));
        std::atomic<unsigned> published = 0;
        std::string previewError;
        std::jthread publisher([&] {
            try {
                for (int i = 0; i < 64; ++i) {
                    (void)iiFileProvider::native::Presentation::attach(nativePath, preview,
                        {{"iteration", std::to_string(i)}});
                    ++published;
                }
            } catch (const std::exception &error) { previewError = error.what(); }
        });
        QString error;
        for (int i = 0; i < 64; ++i)
            CTEST_COMPARE(storage->resolveModel(reference, &error), path);
        publisher.join();
        CTEST_CHECK_MESSAGE(previewError.empty(), previewError.c_str());
        CTEST_COMPARE(published.load(), 64u);
        CTEST_COMPARE(storage->resolveModel(reference, &error), path);
        write(path, "changed model weights");
        CTEST_CHECK(storage->resolveModel(reference, &error).isEmpty());
        CTEST_CHECK(!error.isEmpty());
    }

    void replicaCatalogDoesNotExposeUnindexedLocalModels(bool complete) {
        QTemporaryDir fixture(QDir::current().filePath("replica-model-catalog-XXXXXX.societycontainer")); CTEST_CHECK(fixture.isValid());
        const auto original = SocietyDrive::create(fixture.path()); CTEST_CHECK(original);
        CTEST_CHECK(QDir().mkpath(fixture.filePath("Models/Checkpoint")));
        write(fixture.filePath("Models/Checkpoint/leftover.safetensors"), "unindexed cache");
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto replica = SocietyDrive::adoptReplicaIdentity(fixture.path(), original->identifier(), host); CTEST_CHECK(replica);
        if (complete) CTEST_CHECK(SocietyDrive::completeReplica(fixture.path(), host));
        StorageMap map(*replica); CTEST_CHECK(map.publish({})); CTEST_CHECK(!map.isLocalAuthority());
        StorageModelCatalog catalog; catalog.setDirectory(fixture.filePath("Models"));
        CTEST_WAIT_DEFAULT(!catalog.loading()); CTEST_COMPARE(catalog.count(), 0);
        const auto storage = SharedStorage::open(fixture.path(), nullptr, true); CTEST_CHECK(storage);
        CTEST_CHECK(storage->models().isEmpty());
        CTEST_CHECK(QFileInfo::exists(fixture.filePath("Models/Checkpoint/leftover.safetensors")));
        CTEST_CHECK(map.pendingRequests().isEmpty());
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath(".society-sync/primary.json")));
    }

    void modelCatalogExcludesProviderMetadata(int source) {
        QTemporaryDir fixture(QDir::current().filePath("model-controls-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        const auto base = fixture.filePath("Models");
        QJsonArray objects;
        const auto add = [&](const QString &relative, const QByteArray &bytes) {
            const auto path = QDir(base).filePath(relative);
            CTEST_CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
            write(path, bytes);
            objects.append(QJsonObject{{"path", "models/" + relative}, {"kind", "file"},
                {"size", QString::number(bytes.size())}, {"resident", false}});
        };
        add("Other/example.safetensors", "model bytes");
        add("Other/standalone.json", "{\"description\":\"user asset\"}");
        add("Workflows/graph.json", "{\"nodes\":[]}");
        add("Poses/pose.json", "{\"people\":[]}");
        iiFileProvider::FileCatalog provider(std::filesystem::path(base.toStdU16String()));
        iiFileProvider::FileMetadata annotation; annotation.title = "model metadata";
        CTEST_CHECK(provider.registerFile("Other/example.safetensors", annotation).metadataWritten);
        const auto metadataPath = fixture.filePath("Models/Other/example.safetensors.iisacc.json");
        QFile metadataFile(metadataPath); CTEST_CHECK(metadataFile.open(QIODevice::ReadOnly));
        const auto metadataBytes = metadataFile.readAll(); metadataFile.close();
        CTEST_CHECK(!metadataBytes.isEmpty());
        objects.append(QJsonObject{{"path", "models/Other/example.safetensors.iisacc.json"}, {"kind", "file"},
            {"size", QString::number(metadataBytes.size())}, {"resident", false}});
        add("Other/orphan.IISACC.JSON", "{\"title\":\"unlinked metadata\"}");
        add("Workflows/graph.json.iisacc.json", "{\"title\":\"workflow metadata\"}");
        add("Poses/pose.json.iisacc.json", "{\"title\":\"pose metadata\"}");
        add("Other/example.safetensors.iisacc-preview", "preview metadata");
        StorageMap map(*drive);
        if (source == 2) makeReadyReplica(*drive);
        if (source != 0) CTEST_CHECK(map.publish(objects));
        if (source == 2) CTEST_CHECK(QFile::remove(fixture.filePath("Models/Other/example.safetensors")));

        StorageModelCatalog catalog; catalog.setDirectory(base);
        CTEST_WAIT_VERIFY(!catalog.loading(), 30000);
        CTEST_COMPARE(catalog.count(), 4);
        const auto groups = catalog.groups();
        CTEST_COMPARE(groups.value("Other").toList().size(), 2);
        CTEST_COMPARE(groups.value("Workflows").toList().size(), 1);
        CTEST_COMPARE(groups.value("Poses").toList().size(), 1);
        for (const auto &group : groups) {
            for (const auto &value : group.toList()) {
                const auto relative = value.toMap().value("relativePath").toString();
                CTEST_CHECK_MESSAGE(!iiFileProvider::native::Presentation::isControlFile(
                    std::filesystem::path(relative.toStdU16String())), qPrintable(relative));
            }
        }
        CTEST_CHECK(map.pendingRequests().isEmpty());
        CTEST_CHECK(metadataFile.open(QIODevice::ReadOnly));
        CTEST_COMPARE(metadataFile.readAll(), metadataBytes);
        metadataFile.close();
        CTEST_COMPARE(QFileInfo(fixture.filePath("Models/Other/orphan.IISACC.JSON")).size(),
            qint64(QByteArray("{\"title\":\"unlinked metadata\"}").size()));
    }

    void modelCatalogIgnoresUnchangedRefreshAndDocumentation(bool synchronized) {
        QTemporaryDir fixture(QDir::current().filePath("model-refresh-XXXXXX.societycontainer")); CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        QJsonArray objects;
        const auto add = [&](const QString &relative, const QByteArray &bytes) {
            const auto path = fixture.filePath("Models/" + relative);
            CTEST_CHECK(QDir().mkpath(QFileInfo(path).absolutePath())); write(path, bytes);
            objects.append(QJsonObject{{"path", "models/" + relative}, {"kind", "file"},
                {"size", QString::number(bytes.size())}, {"resident", false}});
        };
        add("VAE/decoder.safetensors", "fixture weights");
        add("VAE/README.md", "documentation"); add("VAE/LICENSE", "license");
        add("VAE/NOTICE.txt", "notice"); add("VAE/config.json", "{}");
        add("Wildcards/colors.txt", "red\nblue");
        StorageMap map(*drive);
        if (synchronized) makeReadyReplica(*drive);
        if (synchronized) CTEST_CHECK(map.publish(objects));
        StorageModelCatalog catalog;
        catalog.setDirectory(fixture.filePath("Models"));
        // The model scans a real filesystem asynchronously; external volumes can
        // take several seconds to publish even this small fixture under I/O load.
        CTEST_WAIT_VERIFY(!catalog.loading(), 30000);
        CTEST_COMPARE(catalog.groups().value("VAE").toList().size(), 1);
        CTEST_COMPARE(catalog.groups().value("Wildcards").toList().size(), 1);
        CTEST_COMPARE(catalog.count(), 2);
        const auto snapshot = catalog.groups();
        society_ctest::SignalRecorder changes(&catalog, &StorageModelCatalog::modelsChanged);
        society_ctest::SignalRecorder before(&catalog, &StorageModelCatalog::modelsAboutToChange);
        society_ctest::SignalRecorder loading(&catalog, &StorageModelCatalog::loadingChanged);
        for (int i = 0; i < 4; ++i) {
            if (synchronized) CTEST_CHECK(map.publish(objects)); // Same rows, rewritten catalog.
            else write(fixture.filePath("Models/VAE/README.md"), QByteArray(i + 1, 'x'));
            catalog.refresh(); catalog.refresh();
            society_ctest::processFor(250);
        }
        CTEST_COMPARE(catalog.groups(), snapshot);
        CTEST_COMPARE(changes.size(), 0); CTEST_COMPARE(before.size(), 0); CTEST_COMPARE(loading.size(), 0);
        add("VAE/new.bin", "new weights");
        if (synchronized) CTEST_CHECK(map.publish(objects));
        catalog.refresh();
        CTEST_WAIT_COMPARE(catalog.count(), 3);
        CTEST_COMPARE(loading.size(), 0);
        CTEST_CHECK(changes.size() > 0);
        CTEST_CHECK(map.pendingRequests().isEmpty());
    }

    void hostModelImportsAppearBeforeSyncHashingCompletes(int iteration) {
        Q_UNUSED(iteration);
        QTemporaryDir fixture(QDir::current().filePath("host-model-import-XXXXXX.societycontainer")); CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        removeEmptyModelCategories(*drive);
        makeReadyReplica(*drive);
        StorageMap map(*drive); CTEST_CHECK(map.publish({}));
        const auto source = fixture.filePath("Models/Other/krea.safetensors");
        CTEST_CHECK(QDir().mkdir(fixture.filePath("Models/Other")));
        write(source, "metadata-only fixture");
        StorageModelCatalog catalog; catalog.setDirectory(fixture.filePath("Models"));
        CTEST_WAIT_DEFAULT(!catalog.loading()); CTEST_COMPARE(catalog.count(), 0); // Replica remains catalog-only.
        write(fixture.filePath(".society-sync/primary.json"), QJsonDocument(QJsonObject{
            {"schema", 1}, {"container", drive->identifier()}, {"scope", QString(64, 'a')}, {"host", "test-host"}}).toJson());
        catalog.refresh(); CTEST_WAIT_COMPARE(catalog.count(), 1);
        const auto destination = fixture.filePath("Models/Checkpoint/krea.safetensors");
        CTEST_CHECK(QDir().mkdir(fixture.filePath("Models/Checkpoint")));
        CTEST_CHECK(QFile::rename(source, destination));
        catalog.refresh(); CTEST_WAIT_COMPARE(catalog.groups().value("Checkpoint").toList().size(), 1);
        CTEST_COMPARE(catalog.groups().value("Other").toList().size(), 0);
        const auto row = catalog.groups().value("Checkpoint").toList().first().toMap();
        CTEST_COMPARE(row.value("bytes").toLongLong(), QFileInfo(destination).size()); CTEST_CHECK(row.value("available").toBool());
        society_ctest::processFor(300);
        society_ctest::SignalRecorder changed(&catalog, &StorageModelCatalog::modelsChanged);
        for (int i = 0; i < 3; ++i) { catalog.refresh(); society_ctest::processFor(200); }
        CTEST_COMPARE(changed.size(), 0); CTEST_CHECK(map.objects().isEmpty()); CTEST_CHECK(map.pendingRequests().isEmpty());
    }
    void modelTypesMatchPhysicalFoldersAndRemoteCatalog() {
        QTemporaryDir fixture(QDir::current().filePath("model-types-XXXXXX.societycontainer")); CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        CTEST_COMPARE(QDir(fixture.filePath("Models")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), allModelTypes().size());
        QJsonArray objects;
        int expected = 0;
        const auto add = [&](const QString &relative, const QByteArray &bytes) {
            const auto path = fixture.filePath("Models/" + relative);
            CTEST_CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
            write(path, bytes);
            objects.append(QJsonObject{{"path", "models/" + relative}, {"kind", "file"},
                {"size", QString::number(bytes.size())}, {"resident", false}, {"version", QString(64, 'a')}});
        };
        for (const auto type : allModelTypes()) {
            const auto name = modelTypeName(type);
            const auto extension = type == ModelType::Wildcards ? ".txt"
                : type == ModelType::Workflows || type == ModelType::ComfyUIWorkflows || type == ModelType::Poses ? ".json" : ".bin";
            add(name + "/sample" + extension, "{}"); ++expected;
            CTEST_CHECK(QFileInfo(fixture.filePath("Models/" + name)).isDir());
        }
        add("Checkpoint/sample.bin.model.json", "{\"society.modality\":\"audio\"}");
        add("Checkpoint/sample.preview.png", "preview");
        add("LLM/package/config.json", "{\"architectures\":[\"LlamaForCausalLM\"]}");
        add("LLM/package/model.bin", "weights"); ++expected;
        add("Checkpoint/pipeline/model_index.json", "{}");
        add("Checkpoint/pipeline/unet/model.bin", "weights"); ++expected;
        add("legacy/unassigned.gguf", "unreadable"); ++expected;
        add("Checkpoint/unsupported.onnx", "preserved");
        add("Other/unsupported.ONNX", "preserved");
        add("Other/.hidden.bin", "hidden");
        StorageModelCatalog local; local.setDirectory(fixture.filePath("Models"));
        CTEST_WAIT_DEFAULT(!local.loading());
        CTEST_COMPARE(local.count(), expected);
        CTEST_COMPARE(local.categories().size(), 23);
        CTEST_COMPARE(local.groups().size(), 23);
        CTEST_COMPARE(local.uncategorizedCount(), 2);
        CTEST_COMPARE(iiFileProvider::native::File::read(std::filesystem::path(fixture.filePath("Models/Checkpoint/unsupported.onnx").toStdU16String())), std::string("preserved"));
        CTEST_COMPARE(iiFileProvider::native::File::read(std::filesystem::path(fixture.filePath("Models/Other/unsupported.ONNX").toStdU16String())), std::string("preserved"));
        for (const auto type : allModelTypes()) {
            const auto name = modelTypeName(type);
            CTEST_COMPARE(local.groups().value(name).toList().size(),
                type == ModelType::Checkpoint || type == ModelType::LLM || type == ModelType::Other ? 2 : 1);
        }
        StorageMap map(*drive); CTEST_CHECK(map.publish(objects));
        // Replicas must use folder identity even with all payloads absent.
        makeReadyReplica(*drive);
        CTEST_CHECK(!map.isLocalAuthority());
        CTEST_CHECK(QDir(fixture.filePath("Models")).removeRecursively());
        CTEST_CHECK(QDir().mkpath(fixture.filePath("Models")));
        StorageModelCatalog remote; remote.setDirectory(fixture.filePath("Models"));
        CTEST_WAIT_DEFAULT(!remote.loading());
        CTEST_COMPARE(remote.count(), expected);
        CTEST_COMPARE(remote.uncategorizedCount(), 2);
        for (const auto type : allModelTypes()) {
            const auto name = modelTypeName(type);
            CTEST_COMPARE(remote.groups().value(name).toList().size(), local.groups().value(name).toList().size());
            for (const auto &value : remote.groups().value(name).toList()) {
                const auto row = value.toMap();
                CTEST_CHECK(!QFileInfo::exists(row.value("path").toString()));
                CTEST_COMPARE(row.value("type").toString(), name);
            }
        }
        CTEST_CHECK(map.pendingRequests().isEmpty());
    }

    void directoryRefreshPublishesOnlyTheChangedRows() {
        QTemporaryDir fixture(QDir::current().filePath("directory-diff-XXXXXX.societycontainer")); CTEST_CHECK(fixture.isValid());
        write(fixture.filePath("b.txt"), "b");
        write(fixture.filePath("c.txt"), "cc");
        StorageDirectoryModel model;
        society_ctest::FlatModelContract modelContract(model);
        model.setFolder(QUrl::fromLocalFile(fixture.path()));
        CTEST_WAIT_COMPARE(model.status(), StorageDirectoryModel::Ready);
        CTEST_COMPARE(model.rowCount(), 2);
        const auto roles = model.roleNames();
        const int pathRole = roles.key("filePath"), sizeRole = roles.key("fileSize");
        QPersistentModelIndex selected(model.index(1));
        society_ctest::SignalCounter resets(&model, &QAbstractItemModel::modelReset);
        society_ctest::SignalRecorder changed(&model, &QAbstractItemModel::dataChanged);
        society_ctest::SignalCounter inserted(&model, &QAbstractItemModel::rowsInserted);
        society_ctest::SignalCounter removed(&model, &QAbstractItemModel::rowsRemoved);
        society_ctest::SignalCounter moved(&model, &QAbstractItemModel::rowsMoved);
        society_ctest::SignalRecorder counts(&model, &StorageDirectoryModel::countChanged);
        society_ctest::SignalRecorder status(&model, &StorageDirectoryModel::statusChanged);

        // Both explicit checks and two automatic polls must remain invisible.
        for (int i = 0; i < 3; ++i) model.refresh();
        society_ctest::processFor(2200);
        CTEST_COMPARE(resets.size(), 0); CTEST_COMPARE(changed.size(), 0);
        CTEST_COMPARE(inserted.size(), 0); CTEST_COMPARE(removed.size(), 0); CTEST_COMPARE(moved.size(), 0);
        CTEST_COMPARE(counts.size(), 0); CTEST_COMPARE(status.size(), 0);

        write(fixture.filePath("b.txt"), "longer content"); model.refresh();
        CTEST_WAIT_COMPARE(model.get(0, "fileSize").toLongLong(), 14);
        CTEST_COMPARE(resets.size(), 0);
        CTEST_COMPARE(changed.size(), 1); CTEST_COMPARE(counts.size(), 0);
        CTEST_COMPARE(changed.first().at(0).value<QModelIndex>().row(), 0);
        CTEST_CHECK(changed.first().at(2).value<QList<int>>().contains(sizeRole));
        CTEST_COMPARE(selected.data(pathRole).toString(), fixture.filePath("c.txt"));

        write(fixture.filePath("a.txt"), "a"); model.refresh();
        CTEST_WAIT_COMPARE(model.rowCount(), 3);
        CTEST_COMPARE(resets.size(), 0); CTEST_COMPARE(inserted.size(), 1); CTEST_COMPARE(counts.size(), 1);
        CTEST_COMPARE(selected.row(), 2);
        CTEST_COMPARE(selected.data(pathRole).toString(), fixture.filePath("c.txt"));
        CTEST_CHECK(QFile::remove(fixture.filePath("b.txt"))); model.refresh();
        CTEST_WAIT_COMPARE(model.rowCount(), 2);
        CTEST_COMPARE(resets.size(), 0); CTEST_COMPARE(removed.size(), 1); CTEST_COMPARE(counts.size(), 2);
        CTEST_COMPARE(selected.row(), 1);

        CTEST_CHECK(model.setProperty("sortReversed", true));
        CTEST_WAIT_COMPARE(model.get(0, "fileName").toString(), "c.txt");
        CTEST_COMPARE(resets.size(), 0); CTEST_CHECK(!moved.isEmpty()); CTEST_COMPARE(counts.size(), 2);
        CTEST_COMPARE(selected.row(), 0);
        CTEST_COMPARE(selected.data(pathRole).toString(), fixture.filePath("c.txt"));
        CTEST_CHECK(QFile::remove(fixture.filePath("c.txt"))); model.refresh();
        CTEST_WAIT_COMPARE(model.rowCount(), 1); CTEST_CHECK(!selected.isValid());
        CTEST_COMPARE(resets.size(), 0);
        CTEST_CHECK_MESSAGE(modelContract.ok(), qPrintable(modelContract.errorString()));
    }
    void hostListingsFollowDeletionBeforeTheCatalogCatchesUp() {
        QTemporaryDir fixture(QDir::current().filePath("host-delete-listing-XXXXXX.societycontainer")); CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive); StorageMap map(*drive);
        const auto source = fixture.filePath("Models/Checkpoint/anima.safetensors");
        CTEST_CHECK(QDir().mkpath(fixture.filePath("Models/Checkpoint")));
        write(source, "fixture");
        const QJsonObject model{{"path", "models/Checkpoint/anima.safetensors"}, {"kind", "file"},
            {"size", "7"}, {"resident", true}, {"version", QString(64, 'a')}};
        CTEST_CHECK(map.publish({model}));
        write(fixture.filePath(".society-sync/primary.json"), QJsonDocument(QJsonObject{
            {"schema", 1}, {"container", drive->identifier()}, {"scope", QString(64, 'a')}, {"host", "test-host"}}).toJson());
        StorageModelCatalog catalog; catalog.setDirectory(fixture.filePath("Models"));
        StorageDirectoryModel original; original.setFolder(QUrl::fromLocalFile(fixture.filePath("Models/Checkpoint")));
        CTEST_WAIT_COMPARE(catalog.count(), 1); CTEST_WAIT_COMPARE(original.rowCount(), 1);
        const auto moved = FileOperations(*drive).perform(FileOperations::Action::Trash, source); CTEST_CHECK(moved);
        catalog.refresh(); original.refresh();
        CTEST_WAIT_COMPARE(catalog.count(), 0); CTEST_WAIT_COMPARE(original.rowCount(), 0);
        // Even an old tombstone cannot hide a newly moved file on the authority.
        const QJsonObject tombstone{{"path", "deleted/anima.safetensors"}, {"kind", "deleted"}};
        CTEST_CHECK(map.publish({model, tombstone}));
        StorageDirectoryModel deleted; deleted.setFolder(QUrl::fromLocalFile(fixture.filePath("Deleted")));
        CTEST_WAIT_COMPARE(deleted.rowCount(), 1);
        auto stale = model; stale["path"] = "deleted/anima.safetensors"; CTEST_CHECK(map.publish({model, stale}));
        deleted.refresh(); CTEST_WAIT_COMPARE(deleted.rowCount(), 1);
        CTEST_CHECK(FileOperations(*drive).perform(FileOperations::Action::Remove, moved.path));
        deleted.refresh(); CTEST_WAIT_COMPARE(deleted.rowCount(), 0);
        CTEST_CHECK(map.pendingRequests().isEmpty());
    }
    void largeDirectoryRefreshReturnsBeforeReadingTheStorageMap() {
        QTemporaryDir fixture(QDir::current().filePath("large-directory-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        makeReadyReplica(*drive);
        StorageMap map(*drive); QJsonArray objects;
        for (int i = 0; i < 8000; ++i)
            objects.append(QJsonObject{{"path", QString("models/Checkpoint/remote-%1.safetensors").arg(i)},
                {"kind", "file"}, {"size", "9000000000"}, {"resident", false}});
        objects.append(QJsonObject{{"path", "files/Documents/visible.txt"}, {"kind", "file"},
            {"size", "123"}, {"resident", false}});
        CTEST_CHECK(map.publish(objects));
        CTEST_CHECK(QDir().mkpath(fixture.filePath("Files/Documents")));
        StorageDirectoryModel model; model.setFolder(QUrl::fromLocalFile(fixture.filePath("Files/Documents")));
        QElapsedTimer elapsed; elapsed.start(); model.refresh();
        CTEST_CHECK_MESSAGE(elapsed.elapsed() < 50, "Directory refresh blocked its GUI caller on the full catalog");
        CTEST_WAIT_COMPARE(model.rowCount(), 1);
        CTEST_COMPARE(model.get(0, "fileName").toString(), "visible.txt");
        CTEST_CHECK(map.pendingRequests().isEmpty());
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("Files/Documents/visible.txt")));
    }
    void largeReplicaModelTreeListsOneCategoryWithoutOpeningPayloads() {
        QTemporaryDir fixture(QDir::current().filePath("large-model-tree-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        removeEmptyModelCategories(*drive);
        makeReadyReplica(*drive);
        StorageMap map(*drive); QJsonArray objects;
        for (int i = 0; i < 8000; ++i)
            objects.append(QJsonObject{{"path", QString("models/Checkpoint/remote-%1.safetensors").arg(i)},
                {"kind", "file"}, {"size", "9000000000"}, {"resident", false}});
        CTEST_CHECK(map.publish(objects));
        StorageDirectoryModel model; model.setFolder(QUrl::fromLocalFile(fixture.filePath("Models")));
        QElapsedTimer elapsed; elapsed.start(); model.refresh();
        CTEST_CHECK_MESSAGE(elapsed.elapsed() < 50, "Model tree refresh blocked its GUI caller on the full catalog");
        CTEST_WAIT_COMPARE_TIMEOUT(model.rowCount(), 1, 5000);
        CTEST_COMPARE(model.get(0, "fileName").toString(), "Checkpoint");
        CTEST_CHECK(model.get(0, "fileIsDir").toBool());
        CTEST_CHECK(map.pendingRequests().isEmpty());
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("Models/Checkpoint")));
    }
    void directoryListsRemoteObjectsAndOpensOnlyCompletePayloads() {
        QTemporaryDir fixture(QDir::current().filePath("remote-directory-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        makeReadyReplica(*drive);
        StorageMap map(*drive);
        QJsonObject object{{"path", "files/Documents/remote.png"}, {"kind", "file"},
            {"size", "3"}, {"version", QString(64, 'a')}, {"hash", QString(64, 'b')}, {"resident", false}};
        CTEST_CHECK(map.publish({object}));
        CTEST_CHECK(QDir().mkpath(fixture.filePath("Files/Documents")));
        StorageDirectoryModel model; model.setFolder(QUrl::fromLocalFile(fixture.filePath("Files/Documents")));
        CTEST_WAIT_COMPARE(model.rowCount(), 1);
        CTEST_COMPARE(model.get(0, "fileName").toString(), "remote.png");
        CTEST_CHECK(!model.get(0, "fileResident").toBool());
        CTEST_CHECK(model.get(0, "filePreviewUrl").toUrl().isEmpty());
        const auto path = map.localPath("files/Documents/remote.png"); CTEST_CHECK(!QFileInfo::exists(path));
        society_ctest::SignalRecorder opened(&model, &StorageDirectoryModel::activated);
        model.activate(0); CTEST_COMPARE(opened.size(), 0); CTEST_WAIT_COMPARE(map.pendingRequests().size(), 1);
        const auto id = map.pendingRequests().first().toObject().value("id").toString();
        write(path, "png"); object["resident"] = true; CTEST_CHECK(map.publish({object}));
        CTEST_CHECK(map.finishRequest(id)); model.refresh(); CTEST_WAIT_COMPARE(opened.size(), 1);
        CTEST_COMPARE(opened.first().first().toString(), path); CTEST_WAIT_DEFAULT(model.get(0, "fileResident").toBool());
        CTEST_CHECK(QFile::remove(path)); model.refresh(); CTEST_WAIT_DEFAULT(!model.get(0, "fileResident").toBool());
        CTEST_COMPARE(model.rowCount(), 1); // Cache absence does not erase the host row.
    }
    void navigationDiscardsAStaleDirectoryRead() {
        QTemporaryDir fixture(QDir::current().filePath("directory-navigation-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        CTEST_CHECK(QDir().mkpath(fixture.filePath("Files/Documents")));
        CTEST_CHECK(QDir().mkpath(fixture.filePath("Files/Audios")));
        write(fixture.filePath("Files/Documents/document.txt"), "doc");
        write(fixture.filePath("Files/Audios/audio.wav"), "audio");
        StorageDirectoryModel model;
        model.setFolder(QUrl::fromLocalFile(fixture.filePath("Files/Documents"))); model.refresh();
        model.setFolder(QUrl::fromLocalFile(fixture.filePath("Files/Audios"))); model.refresh();
        CTEST_WAIT_COMPARE(model.rowCount(), 1);
        CTEST_WAIT_COMPARE(model.get(0, "fileName").toString(), "audio.wav");
        society_ctest::processFor(100); CTEST_COMPARE(model.get(0, "fileName").toString(), "audio.wav");
        model.setFolder({}); CTEST_COMPARE(model.rowCount(), 0); CTEST_COMPARE(model.status(), StorageDirectoryModel::Null);
    }
    void modelCardsCreateOnlyTheSelectedDownloadRequest() {
        QTemporaryDir fixture(QDir::current().filePath("model-cards-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        removeEmptyModelCategories(*drive);
        makeReadyReplica(*drive);
        StorageMap map(*drive);
        QJsonObject selected{{"path", "models/Checkpoint/selected.safetensors"}, {"kind", "file"},
            {"size", "3"}, {"resident", false}, {"version", QString(64, 'a')}};
        auto other = selected; other["path"] = "models/LoRA/unselected.safetensors";
        CTEST_CHECK(map.publish({selected, other}));
        StorageModelCatalog catalog; catalog.setDirectory(fixture.filePath("Models"));
        CTEST_WAIT_COMPARE(catalog.count(), 2); CTEST_WAIT_DEFAULT(!catalog.loading());
        catalog.refresh(); CTEST_WAIT_DEFAULT(!catalog.loading()); CTEST_CHECK(map.pendingRequests().isEmpty());
        const auto path = fixture.filePath("Models/Checkpoint/selected.safetensors");
        CTEST_CHECK(!QFileInfo::exists(path));
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("Models/Checkpoint")));
        society_ctest::SignalRecorder opened(&catalog, &StorageModelCatalog::objectReady);
        QElapsedTimer elapsed; elapsed.start(); catalog.activatePath(path, true);
        CTEST_CHECK(elapsed.elapsed() < 50); CTEST_WAIT_COMPARE(map.pendingRequests().size(), 1);
        const auto request = map.pendingRequests().first().toObject();
        const auto requested = request.value("objects").toArray(); CTEST_COMPARE(requested.size(), 1);
        CTEST_COMPARE(requested.first().toObject().value("path"), selected.value("path"));
        CTEST_COMPARE(requested.first().toObject().value("version"), selected.value("version"));
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("Models/LoRA/unselected.safetensors")));
        CTEST_CHECK(!QFileInfo::exists(fixture.filePath("Models/LoRA")));
        CTEST_CHECK(QDir().mkdir(fixture.filePath("Models/Checkpoint")));
        write(path, "abc"); selected["resident"] = true; CTEST_CHECK(map.publish({selected, other}));
        CTEST_CHECK(map.finishRequest(request.value("id").toString()));
        CTEST_WAIT_COMPARE(opened.size(), 1); CTEST_COMPARE(opened.first().first().toString(), path);
        CTEST_WAIT_COMPARE(catalog.downloadStatus(), QString("Available on this device"));
    }
    void missingModelPackageIsNotReportedReady() {
        QTemporaryDir fixture(QDir::current().filePath("missing-package-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        StorageDirectoryModel opener;
        society_ctest::SignalRecorder ready(&opener, &StorageDirectoryModel::activated);
        society_ctest::SignalRecorder failed(&opener, &StorageDirectoryModel::downloadFailed);
        opener.openPath(fixture.filePath("Models/Checkpoint/removed-package"), true);
        CTEST_WAIT_COMPARE(failed.size(), 1); CTEST_COMPARE(ready.size(), 0);
    }
    void remoteModelCatalogDoesNotCreatePayloads() {
        QTemporaryDir fixture(QDir::current().filePath("remote-models-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path()); CTEST_CHECK(drive);
        makeReadyReplica(*drive);
        StorageMap map(*drive);
        const QJsonObject object{{"path", "models/Checkpoint/remote.safetensors"}, {"kind", "file"},
            {"size", "9000000000"}, {"version", QString(64, 'a')}, {"hash", QString(64, 'b')}, {"resident", false}};
        CTEST_CHECK(map.publish({object}));
        const auto storage = SharedStorage::open(fixture.path()); CTEST_CHECK(storage);
        const auto models = storage->models(); CTEST_COMPARE(models.size(), 1);
        CTEST_COMPARE(models.first().id, "Checkpoint/remote.safetensors"); CTEST_CHECK(!models.first().available);
        CTEST_CHECK(!QFileInfo::exists(map.localPath("models/Checkpoint/remote.safetensors")));
        QString error; CTEST_CHECK(storage->resolveModel(models.first().reference(drive->identifier()), &error).isEmpty());
        CTEST_CHECK(error.contains("Download"));
        CTEST_CHECK(map.request({"models/../outside"}, &error).isEmpty());
        const auto id = map.request({"models/Checkpoint/remote.safetensors"}); CTEST_CHECK(!id.isEmpty());
        CTEST_COMPARE(map.pendingRequests().size(), 1); CTEST_CHECK(map.cancel(id)); CTEST_CHECK(map.pendingRequests().isEmpty());
    }
    void defaultSelectionFollowsHostAdoptionAndConsumersWaitForReadiness()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-mirror-XXXXXX.societycontainer"));
        const auto original = SocietyDrive::create(fixture.path()); CTEST_CHECK(original);
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", fixture.filePath("settings.json").toUtf8());
        qunsetenv("SOCIETY_CONTAINER_PATH");
        CTEST_CHECK(SharedStorage::setDefaultContainer(fixture.path()));
        const auto consumer = SharedStorage::open(); CTEST_CHECK(consumer);
        const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto mirror = SocietyDrive::adoptReplicaIdentity(fixture.path(), original->identifier(), host); CTEST_CHECK(mirror);
        CTEST_CHECK(mirror->isValid()); CTEST_CHECK(!mirror->isReady());
        CTEST_CHECK(!SharedStorage::open());
        const auto owner = SharedStorage::open({}, nullptr, true); CTEST_CHECK(owner);
        CTEST_COMPARE(owner->drive().identifier(), host);
        CTEST_CHECK(owner->filePath(StoreSection::Files, "pending").isEmpty());
        CTEST_CHECK(owner->ensureDirectory(StoreSection::Files, "pending").isEmpty());
        CTEST_CHECK(consumer->filePath(StoreSection::Files, "stale").isEmpty());
        CTEST_CHECK(SocietyDrive::completeReplica(fixture.path(), host));
        const auto reopened = SharedStorage::open(); CTEST_CHECK(reopened); CTEST_COMPARE(reopened->drive().identifier(), host);
        CTEST_CHECK(SharedStorage::setDefaultContainer(fixture.path()));
        const auto replacement = QUuid::createUuid().toString(QUuid::WithoutBraces);
        CTEST_CHECK(SocietyDrive::adoptReplicaIdentity(fixture.path(), host, replacement));
        CTEST_CHECK(SharedStorage::open({}, nullptr, true));
        CTEST_CHECK(SocietyDrive::completeReplica(fixture.path(), replacement));
        CTEST_COMPARE(SharedStorage::open()->drive().identifier(), replacement);
    }
    void rejectsSavedImageSettingsWithoutChangingFilesOrConfiguration()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-legacy-settings-XXXXXX.societycontainer"));
        CTEST_CHECK(fixture.isValid());
        const auto drive = SocietyDrive::createAt(fixture.path()); CTEST_CHECK(drive);
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
        CTEST_CHECK(QDir().mkpath(imagePath + "/bands"));
        write(imagePath + "/bands/preserved", "legacy bytes");
        for (const int version : {1, 2}) {
            const auto configuration = QJsonDocument(QJsonObject{{"schemaVersion", version},
                {"imagePath", imagePath}, {"path", drive->rootPath()},
                {"containerId", drive->identifier()}}).toJson();
            write(settingsPath, configuration);
            QString error;
            CTEST_CHECK(!SharedStorage::open({}, &error));
            CTEST_CHECK(error.contains("not restored"));
            QFile settings(settingsPath); CTEST_CHECK(settings.open(QIODevice::ReadOnly));
            CTEST_COMPARE(settings.readAll(), configuration);
            QFile payload(imagePath + "/bands/preserved"); CTEST_CHECK(payload.open(QIODevice::ReadOnly));
            CTEST_COMPARE(payload.readAll(), QByteArray("legacy bytes"));
        }
        // A valid ordinary folder remains available through an explicit selection.
        CTEST_CHECK(SharedStorage::setDefaultContainer(drive->rootPath()));
        const auto selected = SharedStorage::open(); CTEST_CHECK(selected);
        CTEST_COMPARE(selected->drive().identifier(), drive->identifier());
    }

    void rejectsMisplacedContainersWithoutReplacingSharedSettings()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-nested-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path());
        CTEST_CHECK(drive);
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", fixture.filePath("settings.json").toUtf8());
        qunsetenv("SOCIETY_CONTAINER_PATH");
        CTEST_CHECK(SharedStorage::setDefaultContainer(fixture.path()));
        const auto nested = fixture.filePath("Files");
        for (const auto section : allStoreSections())
            CTEST_CHECK(QDir().mkdir(QDir(nested).filePath(storeSectionName(section))));
        CTEST_CHECK(QFile::copy(fixture.filePath(".society-drive.json"), QDir(nested).filePath(".society-drive.json")));
        QString error;
        CTEST_CHECK(!SharedStorage::setDefaultContainer(nested, &error));
        CTEST_CHECK(!SharedStorage::open(nested, &error));
        const auto unchanged = SharedStorage::open();
        CTEST_CHECK(unchanged);
        CTEST_COMPARE(unchanged->drive().rootPath(), fixture.path());
        CTEST_CHECK(unchanged->filePath(StoreSection::Models, "private.safetensors").startsWith(fixture.filePath("Models/")));
    }

    void nativePathsSupportOrdinaryFilesInEverySection()
    {
        QTemporaryDir fixture(QDir::current().filePath("native-files-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path());
        CTEST_CHECK(drive);
        const auto storage = SharedStorage::open(fixture.path());
        CTEST_CHECK(storage);
        QString error;
        for (const auto section : allStoreSections()) {
            CTEST_COMPARE(storage->filePath(section, {}, &error), drive->sectionPath(section));
            const auto directory = storage->ensureDirectory(section, "Client/.state", &error);
            CTEST_CHECK_MESSAGE(!directory.isEmpty(), qPrintable(error));
            const auto path = storage->filePath(section, "Client/.state/한글 #%.bin", &error);
            CTEST_COMPARE(path, QDir(directory).filePath("한글 #%.bin"));
            CTEST_CHECK(!QFileInfo::exists(path)); // Resolution never creates a file.
            QSaveFile output(path);
            CTEST_CHECK(output.open(QIODevice::WriteOnly));
            CTEST_COMPARE(output.write("shared bytes"), 12);
            CTEST_CHECK(output.commit());
            QFile input(storage->filePath(section, "Client/.state/한글 #%.bin", &error));
            CTEST_CHECK(input.open(QIODevice::ReadOnly));
            CTEST_COMPARE(input.readAll(), QByteArray("shared bytes"));
            CTEST_CHECK(error.isEmpty());
        }
    }

    void nativePathsRejectTraversalAndRedirection()
    {
        QTemporaryDir fixture(QDir::current().filePath("native-boundaries-XXXXXX.societycontainer"));
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto storage = SharedStorage::open(fixture.path());
        CTEST_CHECK(storage);
        const QStringList invalid{"../Files/escape", "/absolute", "a/../../escape", "a//b",
            "a/./b", "a/", "a\\b", "C:alternate", ":/resource", QString("a") + QChar::Null + "b"};
        for (const auto &path : invalid) {
            QString error;
            CTEST_CHECK_MESSAGE(storage->filePath(StoreSection::Models, path, &error).isEmpty(), qPrintable(path));
            CTEST_CHECK(!error.isEmpty());
            CTEST_CHECK(storage->ensureDirectory(StoreSection::Models, path, &error).isEmpty());
        }
        CTEST_COMPARE(QDir(fixture.filePath("Models")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), allModelTypes().size());
        CTEST_CHECK(QDir(fixture.filePath("Models")).entryList(QDir::Files | QDir::Hidden).isEmpty());
        CTEST_CHECK(storage->filePath(StoreSection::Models, "missing/child").isEmpty());
        CTEST_CHECK(storage->filePath(static_cast<StoreSection>(-1), {}).isEmpty());
        write(fixture.filePath("Models/regular"), "file");
        CTEST_CHECK(storage->filePath(StoreSection::Models, "regular/child").isEmpty());
#ifdef Q_OS_UNIX
        CTEST_CHECK(createTestSymbolicLink(fixture.filePath("Files"), fixture.filePath("Models/redirect")));
        CTEST_CHECK(createTestSymbolicLink(fixture.filePath("not-present"), fixture.filePath("Models/dangling")));
        for (const auto &path : {"redirect", "redirect/escape", "dangling"})
            CTEST_CHECK(storage->filePath(StoreSection::Models, path).isEmpty());
        CTEST_COMPARE(QDir(fixture.filePath("Files")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
#endif
    }

    void pinnedStorageRejectsRootRedirectAndIdentityReplacement()
    {
        QTemporaryDir fixture(QDir::current().filePath("native-identity-XXXXXX"));
        const auto root = fixture.filePath("Society.societycontainer");
        CTEST_CHECK(QDir().mkdir(root));
        CTEST_CHECK(SocietyDrive::create(root));
        const auto storage = SharedStorage::open(root);
        CTEST_CHECK(storage);
#ifdef Q_OS_UNIX
        const auto moved = fixture.filePath("Moved");
        CTEST_CHECK(QDir().rename(root, moved));
        CTEST_CHECK(createTestSymbolicLink(moved, root));
        CTEST_CHECK(!storage->drive().isValid());
        CTEST_CHECK(storage->filePath(StoreSection::Files, "file").isEmpty());
        CTEST_CHECK(QFile::remove(root));
        CTEST_CHECK(QDir().rename(moved, root));
#endif
        CTEST_CHECK(QFile::remove(QDir(root).filePath(".society-drive.json")));
        CTEST_CHECK(SocietyDrive::create(root));
        CTEST_CHECK(storage->filePath(StoreSection::Files, "file").isEmpty());
        CTEST_CHECK(storage->ensureDirectory(StoreSection::Models, "new").isEmpty());
    }

    void appsDiscoverTheSameDriveAndCannotSilentlySwitchIdentity()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-storage-XXXXXX"));
        CTEST_CHECK(QDir().mkdir(fixture.filePath("Society.societycontainer")));
        const auto drive = SocietyDrive::create(fixture.filePath("Society.societycontainer"));
        CTEST_CHECK(drive);
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", fixture.filePath("settings.json").toUtf8());
        qunsetenv("SOCIETY_CONTAINER_PATH");
        QString error;
        QCoreApplication::setApplicationName("Society");
        CTEST_CHECK_MESSAGE(SharedStorage::setDefaultContainer(drive->rootPath(), &error), qPrintable(error));
        QCoreApplication::setApplicationName("Dreamscapes");
        const auto client = SharedStorage::open({}, &error);
        CTEST_CHECK_MESSAGE(client.has_value(), qPrintable(error));
        CTEST_COMPARE(client->drive().identifier(), drive->identifier());
        CTEST_COMPARE(client->drive().sectionPath(StoreSection::Models), fixture.filePath("Society.societycontainer/Models"));
        CTEST_CHECK(QFile::remove(fixture.filePath("Society.societycontainer/.society-drive.json")));
        CTEST_CHECK(SocietyDrive::create(drive->rootPath()));
        CTEST_CHECK(!SharedStorage::open({}, &error));
        CTEST_CHECK(error.contains("identity"));
    }

    void referencesResolveOnlyWithinTheSameModelsArea()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-models-XXXXXX.societycontainer"));
        const auto drive = SocietyDrive::create(fixture.path());
        CTEST_CHECK(drive);
        write(fixture.filePath("Models/weights.safetensors"), "one");
        write(fixture.filePath("Files/public.safetensors"), "public");
        write(fixture.filePath("Models/.partial.safetensors"), "partial");
        CTEST_CHECK(QDir().mkdir(fixture.filePath("Models/pipeline")));
        write(fixture.filePath("Models/pipeline/model_index.json"), "{\"_class_name\":\"StableDiffusionPipeline\"}");
        write(fixture.filePath("Models/pipeline/model.safetensors"), "weights");
        CTEST_CHECK(QDir().mkpath(fixture.filePath("Models/cascade.iildmodel/members")));
        write(fixture.filePath("Models/cascade.iildmodel/model_index.json"),
              "{\"schema\":\"iild-unified-model-v1\",\"_class_name\":\"IILDUnifiedCascade\"}");
        write(fixture.filePath("Models/cascade.iildmodel/members/model.safetensors"), "member weights");
        const auto client = SharedStorage::open(fixture.path());
        CTEST_CHECK(client);
        const auto models = client->models();
        CTEST_COMPARE(models.size(), 3);
        const auto unified = std::find_if(models.begin(), models.end(), [](const auto &m) { return m.format == "unified"; });
        CTEST_CHECK(unified != models.end());
        CTEST_COMPARE(unified->id, QString("cascade.iildmodel"));
        for (const auto &model : models) {
            auto reference = model.reference(drive->identifier());
            CTEST_COMPARE(client->resolveModel(reference), fixture.filePath("Models/" + model.id));
            reference["containerId"] = "another-drive";
            CTEST_CHECK(client->resolveModel(reference).isEmpty());
        }
        const auto file = *std::find_if(models.begin(), models.end(), [](const auto &m) { return m.format == "safetensors"; });
        auto reference = file.reference(drive->identifier());
        reference["path"] = "../Files/public.safetensors";
        CTEST_CHECK(client->resolveModel(reference).isEmpty());
        write(fixture.filePath("Models/weights.safetensors"), "changed model");
        CTEST_CHECK(client->resolveModel(file.reference(drive->identifier())).isEmpty());
        write(fixture.filePath("Models/cascade.iildmodel/members/model.safetensors"), "changed member");
        CTEST_CHECK(client->resolveModel(unified->reference(drive->identifier())).isEmpty());
    }

    void privateOutputDirectoriesRejectRedirection()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-outputs-XXXXXX.societycontainer"));
        CTEST_CHECK(SocietyDrive::create(fixture.path()));
        const auto client = SharedStorage::open(fixture.path());
        CTEST_CHECK(client);
        CTEST_COMPARE(client->filePath(StoreSection::GenerationHistory, "unique-result.png"),
                 fixture.filePath("Generation History/unique-result.png"));
        CTEST_CHECK(QDir(fixture.filePath("Generation History")).isEmpty());
        CTEST_COMPARE(client->ensureDirectory(StoreSection::AssetLibrary, "project"),
                 fixture.filePath("Asset Library/project"));
        CTEST_CHECK(client->ensureDirectory(StoreSection::Models, "../Files/escape").isEmpty());
        CTEST_CHECK(createTestSymbolicLink(fixture.filePath("Files"), fixture.filePath("Asset Library/redirect")));
        CTEST_CHECK(client->ensureDirectory(StoreSection::AssetLibrary, "redirect/job").isEmpty());
        CTEST_COMPARE(QDir(fixture.filePath("Files")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    society_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    SharedStorageTests tests;
    const auto scoped = [&](auto&& action) {
        const auto previousSettings = qgetenv("SOCIETY_STORAGE_SETTINGS_PATH");
        const auto previousContainer = qgetenv("SOCIETY_CONTAINER_PATH");
        const auto restore = qScopeGuard([&] {
            previousSettings.isNull() ? qunsetenv("SOCIETY_STORAGE_SETTINGS_PATH") : qputenv("SOCIETY_STORAGE_SETTINGS_PATH", previousSettings);
            previousContainer.isNull() ? qunsetenv("SOCIETY_CONTAINER_PATH") : qputenv("SOCIETY_CONTAINER_PATH", previousContainer);
        });
        struct FlushDeferredDeletes {
            ~FlushDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
        } cleanup;
        action();
    };
    runner.run("emptyModelCategoriesAppearInLocalAndReplicaListings:local", [&] { scoped([&] { tests.emptyModelCategoriesAppearInLocalAndReplicaListings(false); }); });
    runner.run("emptyModelCategoriesAppearInLocalAndReplicaListings:replica", [&] { scoped([&] { tests.emptyModelCategoriesAppearInLocalAndReplicaListings(true); }); });
    runner.run("legacyClassificationsAreIsolatedWithoutMergingUnknownFiles", [&] { scoped([&] { tests.legacyClassificationsAreIsolatedWithoutMergingUnknownFiles(); }); });
    runner.run("replacedExternalFileDoesNotInheritLegacyClassification", [&] { scoped([&] { tests.replacedExternalFileDoesNotInheritLegacyClassification(); }); });
    runner.run("stalePrivateClassificationDoesNotAttachToAReusedPath", [&] { scoped([&] { tests.stalePrivateClassificationDoesNotAttachToAReusedPath(); }); });
    runner.run("directorySearchAndTypeSortUseCurrentOptions", [&] { scoped([&] { tests.directorySearchAndTypeSortUseCurrentOptions(); }); });
    runner.run("modelDirectoryHidesAttachedCompanionsInLiveAndCachedRows", [&] { scoped([&] { tests.modelDirectoryHidesAttachedCompanionsInLiveAndCachedRows(); }); });
    runner.run("replicaModelDirectoryHidesCompanionsWithoutDownloading", [&] { scoped([&] { tests.replicaModelDirectoryHidesCompanionsWithoutDownloading(); }); });
    runner.run("replicaModelTreeNavigatesCatalogDirectoriesWithoutDownloading", [&] { scoped([&] { tests.replicaModelTreeNavigatesCatalogDirectoriesWithoutDownloading(); }); });
    runner.run("replicaModelTreeRejectsDeletedAndRedirectedFolders", [&] { scoped([&] { tests.replicaModelTreeRejectsDeletedAndRedirectedFolders(); }); });
    runner.run("replicaModelTreeRechecksDeletedDescendantsBeforeOpening", [&] { scoped([&] { tests.replicaModelTreeRechecksDeletedDescendantsBeforeOpening(); }); });
    runner.run("staleDirectorySnapshotNeverExposesProviderMetadata", [&] { scoped([&] { tests.staleDirectorySnapshotNeverExposesProviderMetadata(); }); });
    runner.run("daemonInventoryRestoresBeforeLiveValidation", [&] { scoped([&] { tests.daemonInventoryRestoresBeforeLiveValidation(); }); });
    runner.run("modelInventoryExcludesOnnxAndKeepsGguf", [&] { scoped([&] { tests.modelInventoryExcludesOnnxAndKeepsGguf(); }); });
    runner.run("fileInformationExposesProviderPresentationAndClassification", [&] { scoped([&] { tests.fileInformationExposesProviderPresentationAndClassification(); }); });
    runner.run("externalFilesReceiveProviderDetailsAndPreviewWithoutAnAppSave", [&] { scoped([&] { tests.externalFilesReceiveProviderDetailsAndPreviewWithoutAnAppSave(); }); });
    runner.run("generatedPngPublicationPreservesBytesAndRejectsCollisionsAndRedirects", [&] { scoped([&] { tests.generatedPngPublicationPreservesBytesAndRejectsCollisionsAndRedirects(); }); });
    runner.run("runtimePathsStayInsideTheSelectedPackageAndCreateOnlyOnRequest", [&] { scoped([&] { tests.runtimePathsStayInsideTheSelectedPackageAndCreateOnlyOnRequest(); }); });
    runner.run("cachedDirectorySnapshotIsReconciledBeforeOpening", [&] { scoped([&] { tests.cachedDirectorySnapshotIsReconciledBeforeOpening(); }); });
    runner.run("generationInventoryReconcilesAuthorityAndPreservesReplicas", [&] { scoped([&] { tests.generationInventoryReconcilesAuthorityAndPreservesReplicas(); }); });
    runner.run("originalModelFilesRemainVisibleWithAPartialCatalogAndNoRegisteredHost", [&] { scoped([&] { tests.originalModelFilesRemainVisibleWithAPartialCatalogAndNoRegisteredHost(); }); });
    runner.run("localModelReferenceRemainsStableWhileProviderPublishesPreviews", [&] { scoped([&] { tests.localModelReferenceRemainsStableWhileProviderPublishesPreviews(); }); });
    runner.run("replicaCatalogDoesNotExposeUnindexedLocalModels:incomplete", [&] { scoped([&] { tests.replicaCatalogDoesNotExposeUnindexedLocalModels(false); }); });
    runner.run("replicaCatalogDoesNotExposeUnindexedLocalModels:complete", [&] { scoped([&] { tests.replicaCatalogDoesNotExposeUnindexedLocalModels(true); }); });
    runner.run("modelCatalogExcludesProviderMetadata:filesystem", [&] { scoped([&] { tests.modelCatalogExcludesProviderMetadata(0); }); });
    runner.run("modelCatalogExcludesProviderMetadata:local-authority", [&] { scoped([&] { tests.modelCatalogExcludesProviderMetadata(1); }); });
    runner.run("modelCatalogExcludesProviderMetadata:remote-replica", [&] { scoped([&] { tests.modelCatalogExcludesProviderMetadata(2); }); });
    runner.run("modelCatalogIgnoresUnchangedRefreshAndDocumentation:filesystem", [&] { scoped([&] { tests.modelCatalogIgnoresUnchangedRefreshAndDocumentation(false); }); });
    runner.run("modelCatalogIgnoresUnchangedRefreshAndDocumentation:replica-catalog", [&] { scoped([&] { tests.modelCatalogIgnoresUnchangedRefreshAndDocumentation(true); }); });
    for (int iteration = 0; iteration < 16; ++iteration)
        runner.run("hostModelImportsAppearBeforeSyncHashingCompletes:" + std::to_string(iteration), [&] { scoped([&] { tests.hostModelImportsAppearBeforeSyncHashingCompletes(iteration); }); });
    runner.run("modelTypesMatchPhysicalFoldersAndRemoteCatalog", [&] { scoped([&] { tests.modelTypesMatchPhysicalFoldersAndRemoteCatalog(); }); });
    runner.run("directoryRefreshPublishesOnlyTheChangedRows", [&] { scoped([&] { tests.directoryRefreshPublishesOnlyTheChangedRows(); }); });
    runner.run("hostListingsFollowDeletionBeforeTheCatalogCatchesUp", [&] { scoped([&] { tests.hostListingsFollowDeletionBeforeTheCatalogCatchesUp(); }); });
    runner.run("largeDirectoryRefreshReturnsBeforeReadingTheStorageMap", [&] { scoped([&] { tests.largeDirectoryRefreshReturnsBeforeReadingTheStorageMap(); }); });
    runner.run("largeReplicaModelTreeListsOneCategoryWithoutOpeningPayloads", [&] { scoped([&] { tests.largeReplicaModelTreeListsOneCategoryWithoutOpeningPayloads(); }); });
    runner.run("directoryListsRemoteObjectsAndOpensOnlyCompletePayloads", [&] { scoped([&] { tests.directoryListsRemoteObjectsAndOpensOnlyCompletePayloads(); }); });
    runner.run("navigationDiscardsAStaleDirectoryRead", [&] { scoped([&] { tests.navigationDiscardsAStaleDirectoryRead(); }); });
    runner.run("modelCardsCreateOnlyTheSelectedDownloadRequest", [&] { scoped([&] { tests.modelCardsCreateOnlyTheSelectedDownloadRequest(); }); });
    runner.run("missingModelPackageIsNotReportedReady", [&] { scoped([&] { tests.missingModelPackageIsNotReportedReady(); }); });
    runner.run("remoteModelCatalogDoesNotCreatePayloads", [&] { scoped([&] { tests.remoteModelCatalogDoesNotCreatePayloads(); }); });
    runner.run("defaultSelectionFollowsHostAdoptionAndConsumersWaitForReadiness", [&] { scoped([&] { tests.defaultSelectionFollowsHostAdoptionAndConsumersWaitForReadiness(); }); });
    runner.run("rejectsSavedImageSettingsWithoutChangingFilesOrConfiguration", [&] { scoped([&] { tests.rejectsSavedImageSettingsWithoutChangingFilesOrConfiguration(); }); });
    runner.run("rejectsMisplacedContainersWithoutReplacingSharedSettings", [&] { scoped([&] { tests.rejectsMisplacedContainersWithoutReplacingSharedSettings(); }); });
    runner.run("nativePathsSupportOrdinaryFilesInEverySection", [&] { scoped([&] { tests.nativePathsSupportOrdinaryFilesInEverySection(); }); });
    runner.run("nativePathsRejectTraversalAndRedirection", [&] { scoped([&] { tests.nativePathsRejectTraversalAndRedirection(); }); });
    runner.run("pinnedStorageRejectsRootRedirectAndIdentityReplacement", [&] { scoped([&] { tests.pinnedStorageRejectsRootRedirectAndIdentityReplacement(); }); });
    runner.run("appsDiscoverTheSameDriveAndCannotSilentlySwitchIdentity", [&] { scoped([&] { tests.appsDiscoverTheSameDriveAndCannotSilentlySwitchIdentity(); }); });
    runner.run("referencesResolveOnlyWithinTheSameModelsArea", [&] { scoped([&] { tests.referencesResolveOnlyWithinTheSameModelsArea(); }); });
    runner.run("privateOutputDirectoriesRejectRedirection", [&] { scoped([&] { tests.privateOutputDirectoriesRejectRedirection(); }); });
    return runner.result();
}
