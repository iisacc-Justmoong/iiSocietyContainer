#include <SharedStorage.h>
#include <algorithm>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace iiSocietyContainer;

namespace {
void write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}
}

class SharedStorageTests : public QObject
{
    Q_OBJECT
private slots:
    void defaultSelectionFollowsHostAdoptionAndConsumersWaitForReadiness()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-mirror-XXXXXX"));
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
    void rejectsMisplacedContainersWithoutReplacingSharedSettings()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-nested-XXXXXX"));
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
        QTemporaryDir fixture(QDir::current().filePath("native-files-XXXXXX"));
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
        QTemporaryDir fixture(QDir::current().filePath("native-boundaries-XXXXXX"));
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
        QCOMPARE(QDir(fixture.filePath("Models")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 23);
        QVERIFY(QDir(fixture.filePath("Models")).entryList(QDir::Files | QDir::Hidden).isEmpty());
        QVERIFY(storage->filePath(StoreSection::Models, "missing/child").isEmpty());
        QVERIFY(storage->filePath(static_cast<StoreSection>(-1), {}).isEmpty());
        write(fixture.filePath("Models/regular"), "file");
        QVERIFY(storage->filePath(StoreSection::Models, "regular/child").isEmpty());
#ifdef Q_OS_UNIX
        QVERIFY(QFile::link(fixture.filePath("Files"), fixture.filePath("Models/redirect")));
        QVERIFY(QFile::link(fixture.filePath("not-present"), fixture.filePath("Models/dangling")));
        for (const auto &path : {"redirect", "redirect/escape", "dangling"})
            QVERIFY(storage->filePath(StoreSection::Models, path).isEmpty());
        QCOMPARE(QDir(fixture.filePath("Files")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 3);
#endif
    }

    void pinnedStorageRejectsRootRedirectAndIdentityReplacement()
    {
        QTemporaryDir fixture(QDir::current().filePath("native-identity-XXXXXX"));
        const auto root = fixture.filePath("Society");
        QVERIFY(QDir().mkdir(root));
        QVERIFY(SocietyDrive::create(root));
        const auto storage = SharedStorage::open(root);
        QVERIFY(storage);
#ifdef Q_OS_UNIX
        const auto moved = fixture.filePath("Moved");
        QVERIFY(QDir().rename(root, moved));
        QVERIFY(QFile::link(moved, root));
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
        QVERIFY(QDir().mkdir(fixture.filePath("Society")));
        const auto drive = SocietyDrive::create(fixture.filePath("Society"));
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
        QCOMPARE(client->drive().sectionPath(StoreSection::Models), fixture.filePath("Society/Models"));
        QVERIFY(QFile::remove(fixture.filePath("Society/.society-drive.json")));
        QVERIFY(SocietyDrive::create(drive->rootPath()));
        QVERIFY(!SharedStorage::open({}, &error));
        QVERIFY(error.contains("identity"));
    }

    void referencesResolveOnlyWithinTheSameModelsArea()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-models-XXXXXX"));
        const auto drive = SocietyDrive::create(fixture.path());
        QVERIFY(drive);
        write(fixture.filePath("Models/weights.safetensor"), "one");
        write(fixture.filePath("Files/public.safetensors"), "public");
        write(fixture.filePath("Models/.partial.safetensors"), "partial");
        QVERIFY(QDir().mkdir(fixture.filePath("Models/pipeline")));
        write(fixture.filePath("Models/pipeline/model_index.json"), "{\"_class_name\":\"StableDiffusionPipeline\"}");
        write(fixture.filePath("Models/pipeline/model.safetensors"), "weights");
        const auto client = SharedStorage::open(fixture.path());
        QVERIFY(client);
        const auto models = client->models();
        QCOMPARE(models.size(), 2);
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
    }

    void privateOutputDirectoriesRejectRedirection()
    {
        QTemporaryDir fixture(QDir::current().filePath("shared-outputs-XXXXXX"));
        QVERIFY(SocietyDrive::create(fixture.path()));
        const auto client = SharedStorage::open(fixture.path());
        QVERIFY(client);
        QCOMPARE(client->filePath(StoreSection::GenerationHistory, "unique-result.png"),
                 fixture.filePath("Generation History/unique-result.png"));
        QVERIFY(QDir(fixture.filePath("Generation History")).isEmpty());
        QCOMPARE(client->ensureDirectory(StoreSection::AssetLibrary, "project"),
                 fixture.filePath("Asset Library/project"));
        QVERIFY(client->ensureDirectory(StoreSection::Models, "../Files/escape").isEmpty());
        QVERIFY(QFile::link(fixture.filePath("Files"), fixture.filePath("Asset Library/redirect")));
        QVERIFY(client->ensureDirectory(StoreSection::AssetLibrary, "redirect/job").isEmpty());
        QCOMPARE(QDir(fixture.filePath("Files")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 3);
    }
};

QTEST_GUILESS_MAIN(SharedStorageTests)
#include "shared_storage.moc"
