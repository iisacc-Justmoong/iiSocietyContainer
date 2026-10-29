#include <ApplicationStorage.h>
#include <LogicalDisk.h>
#include <SharedStorage.h>
#include <StorageMaintenance.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <array>

using namespace iiSocietyContainer;
namespace {
void put(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size());
}
QByteArray get(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
class IsolatedSelection final {
public:
    explicit IsolatedSelection(const QString &directory) {
        for (std::size_t i = 0; i < names.size(); ++i) { values[i] = qgetenv(names[i]); qunsetenv(names[i]); }
        qputenv("SOCIETY_STORAGE_SETTINGS_PATH", (directory + "/selection.json").toUtf8());
        qputenv("IISACC_SHARED_STORAGE_ROOT", (directory + "/suite").toUtf8());
    }
    ~IsolatedSelection() {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (values[i].isNull()) qunsetenv(names[i]); else qputenv(names[i], values[i]);
        QCoreApplication::setApplicationName(applicationName);
    }
private:
    const std::array<const char *, 3> names{"SOCIETY_CONTAINER_PATH", "SOCIETY_STORAGE_SETTINGS_PATH", "IISACC_SHARED_STORAGE_ROOT"};
    std::array<QByteArray, 3> values;
    const QString applicationName = QCoreApplication::applicationName();
};
}
class LogicalStorageTests final : public QObject {
    Q_OBJECT
private slots:
    void nativeDesktopRootIsIndependentOfApplicationMetadata() {
#if defined(Q_OS_WIN) || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID))
        QTemporaryDir directory(LOGICAL_STORAGE_TEST_DIRECTORY "/suite-native-XXXXXX"); QVERIFY(directory.isValid());
        IsolatedSelection selection(directory.path()); qunsetenv("IISACC_SHARED_STORAGE_ROOT"); QString error;
        const auto expected = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("iisacc/Society");
        QCOMPARE(ApplicationStorage::sharedRoot(&error), expected); QVERIFY2(error.isEmpty(), qPrintable(error));
        QCoreApplication::setApplicationName("Another iisacc product");
        QCOMPARE(ApplicationStorage::sharedRoot(&error), expected);
#else
        QSKIP("Native user data root selection requires Windows or Linux.");
#endif
    }
    void suiteDiscoveryIsIndependentOfTheProductAndReadersDoNotBootstrap() {
        QTemporaryDir directory(LOGICAL_STORAGE_TEST_DIRECTORY "/suite-XXXXXX"); QVERIFY(directory.isValid());
        IsolatedSelection selection(directory.path()); QString error;
        const auto root = directory.filePath("suite");
        QVERIFY(!SharedStorage::open({}, &error)); QVERIFY(!QFileInfo::exists(root));
        const auto first = ApplicationStorage::openForApplication("com.iisacc.dreamscapes", &error);
        QVERIFY2(first, qPrintable(error));
        QCOMPARE(ApplicationStorage::sharedRoot(&error), root);
        QCOMPARE(ApplicationStorage::sharedGroupRoot(&error), root);
        const auto model = first->filePath(StoreSection::Models, "shared.safetensors"); put(model, "suite model");
        QCoreApplication::setApplicationName("Another iisacc product");
        const auto reader = SharedStorage::open({}, &error); QVERIFY2(reader, qPrintable(error));
        const auto second = ApplicationStorage::openForApplication("com.iisacc.another", &error); QVERIFY2(second, qPrintable(error));
        QCOMPARE(reader->drive().identifier(), first->drive().identifier());
        QCOMPARE(second->drive().rootPath(), first->drive().rootPath());
        QCOMPARE(second->drive().ownerApplication(), "com.iisacc.dreamscapes");
        QCOMPARE(second->filePath(StoreSection::Models, "shared.safetensors"), model);
        QCOMPARE(get(model), "suite model");
        QCOMPARE(ApplicationStorage::registration(root, &error).value("ownerApplication"), "com.iisacc.dreamscapes");
    }
    void explicitSelectionsAndInvalidSuiteRootsNeverForkStorage() {
        QTemporaryDir directory(LOGICAL_STORAGE_TEST_DIRECTORY "/suite-selection-XXXXXX"); QVERIFY(directory.isValid());
        IsolatedSelection selection(directory.path()); QString error;
        const auto drive = SocietyDrive::createAt(directory.path(), &error); QVERIFY2(drive, qPrintable(error));
        qputenv("SOCIETY_CONTAINER_PATH", drive->rootPath().toUtf8());
        const auto existing = ApplicationStorage::openForApplication("com.iisacc.another", &error); QVERIFY2(existing, qPrintable(error));
        QCOMPARE(existing->drive().rootPath(), drive->rootPath()); QVERIFY(!QFileInfo::exists(directory.filePath("suite")));
        qunsetenv("SOCIETY_CONTAINER_PATH");
        put(directory.filePath("selection.json"), "{broken");
        QVERIFY(!ApplicationStorage::openForApplication("com.iisacc.another", &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(directory.filePath("suite"))); QVERIFY(QFile::remove(directory.filePath("selection.json")));
        for (const auto &invalid : {QByteArray(""), QByteArray("relative-suite")}) {
            qputenv("IISACC_SHARED_STORAGE_ROOT", invalid);
            // Some Windows runtimes remove a variable when assigned an empty
            // value. Never let this negative fixture bootstrap a real default.
            if (!qEnvironmentVariableIsSet("IISACC_SHARED_STORAGE_ROOT")) continue;
            QVERIFY(!ApplicationStorage::openForApplication("com.iisacc.another", &error)); QVERIFY(!error.isEmpty());
            QVERIFY(!QFileInfo::exists(directory.filePath("suite")));
        }
    }
    void redirectedSuiteRootsNeverTouchAnotherDirectory() {
        QTemporaryDir directory(LOGICAL_STORAGE_TEST_DIRECTORY "/suite-redirect-XXXXXX"); QVERIFY(directory.isValid());
        IsolatedSelection selection(directory.path()); QString error;
        const auto outside = directory.filePath("outside"); QVERIFY(QDir().mkdir(outside));
        std::error_code linkError;
        std::filesystem::create_directory_symlink(std::filesystem::path(outside.toStdU16String()),
            std::filesystem::path(directory.filePath("suite").toStdU16String()), linkError);
        if (linkError) QSKIP("This environment does not permit directory symlinks.");
        QVERIFY(!ApplicationStorage::openForApplication("com.iisacc.another", &error)); QVERIFY(!error.isEmpty());
        QVERIFY(QDir(outside).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    }
    void sharedGuardIsPrivateAndRejectsRedirection() {
#if defined(Q_OS_DARWIN)
        QTemporaryDir group(LOGICAL_STORAGE_TEST_DIRECTORY "/guard-XXXXXX"); QVERIFY(group.isValid());
        QTemporaryDir outside(LOGICAL_STORAGE_TEST_DIRECTORY "/guard-outside-XXXXXX"); QVERIFY(outside.isValid());
        const auto sentinel = outside.filePath("sentinel"); put(sentinel, "unchanged");
        const auto guard = group.filePath(".iisacc-storage.guard");
        QVERIFY(QFile::link(sentinel, guard)); QString error;
        QVERIFY(!ApplicationStorage::openInGroup(group.path(), "com.iisacc.guard-test", &error));
        QVERIFY(!error.isEmpty()); QCOMPARE(get(sentinel), QByteArray("unchanged"));
        QVERIFY(QFile::remove(guard));
        QVERIFY2(ApplicationStorage::openInGroup(group.path(), "com.iisacc.guard-test", &error), qPrintable(error));
        const auto permissions = QFile::permissions(guard);
        QVERIFY(!(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)));
        QCOMPARE(QFileInfo(guard).size(), qint64(0)); QCOMPARE(get(sentinel), QByteArray("unchanged"));
#else
        QSKIP("The shared Apple ownership guard is platform-specific.");
#endif
    }
    void applicationOwnsPersistentLogicalDiskWithoutAPackageOrSociety() {
        QTemporaryDir group(LOGICAL_STORAGE_TEST_DIRECTORY "/group-XXXXXX"); QVERIFY(group.isValid()); QString error;
        const auto storage = ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error);
        QVERIFY2(storage, qPrintable(error)); const auto drive = storage->drive();
        QCOMPARE(drive.storageKind(), SocietyDrive::StorageKind::LogicalDisk);
        QCOMPARE(drive.ownerApplication(), "com.iisacc.dreamscapes");
        QVERIFY(!drive.rootPath().endsWith(".societycontainer")); QVERIFY(!QFileInfo::exists(group.filePath("Society.societycontainer")));
        QVERIFY(drive.isReady()); QCOMPARE(drive.sections().size(), 9);
        const auto reused = SocietyDrive::createAt(drive.rootPath(), &error); QVERIFY2(reused, qPrintable(error));
        QCOMPARE(reused->identifier(), drive.identifier());
        const auto model = storage->filePath(StoreSection::Models, "fixture.safetensors", &error); QVERIFY2(!model.isEmpty(), qPrintable(error));
        put(model, "model download bytes"); const auto image = storage->filePath(StoreSection::GenerationHistory, "generated.png", &error);
        put(image, "generated image bytes"); const auto identifier = drive.identifier();
        const auto reopened = ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error);
        QVERIFY2(reopened, qPrintable(error)); QCOMPARE(reopened->drive().identifier(), identifier);
        QCOMPARE(get(model), "model download bytes"); QCOMPARE(get(image), "generated image bytes");
        QCOMPARE(ApplicationStorage::registration(group.path(), &error).value("managingApplication"), "com.iisacc.dreamscapes");
    }
    void societyAdoptsTheSameDiskAndPreservesPathsIdentityReferencesAndManagedHistory() {
        QTemporaryDir group(LOGICAL_STORAGE_TEST_DIRECTORY "/adoption-XXXXXX"); QString error;
        const auto before = ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error); QVERIFY2(before, qPrintable(error));
        const auto model = before->filePath(StoreSection::Models, "fixture.safetensors"); put(model, "original model");
        const auto image = before->filePath(StoreSection::GenerationHistory, "generated.png"); put(image, "generated image");
        QVERIFY2(before->recordFile(StoreSection::GenerationHistory, "generated.png", &error), qPrintable(error));
        const auto tracking = before->fileTracking(StoreSection::GenerationHistory, "generated.png", &error); QVERIFY2(!tracking.isEmpty(), qPrintable(error));
        const auto reference = before->models().first().reference(before->drive().identifier());
        const auto adopted = ApplicationStorage::adoptInGroup(group.path(), &error); QVERIFY2(adopted, qPrintable(error));
        QCOMPARE(adopted->drive().rootPath(), before->drive().rootPath()); QCOMPARE(adopted->drive().identifier(), before->drive().identifier());
        QVERIFY(before->drive().isReady()); QCOMPARE(adopted->resolveModel(reference, &error), model);
        QCOMPARE(adopted->fileTracking(StoreSection::GenerationHistory, "generated.png", &error), tracking);
        QCOMPARE(get(image), "generated image");
        QCOMPARE(ApplicationStorage::registration(group.path(), &error).value("managingApplication"), "com.iisacc.society");
        const auto after = ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error); QVERIFY(after);
        QCOMPARE(after->drive().identifier(), before->drive().identifier());
        QCOMPARE(ApplicationStorage::registration(group.path(), &error).value("managingApplication"), "com.iisacc.society");
    }
    void existingPackageWinsWithoutCreatingAnotherDisk() {
        QTemporaryDir group(LOGICAL_STORAGE_TEST_DIRECTORY "/package-XXXXXX"); QString error;
        const auto drive = SocietyDrive::createAt(group.path(), &error); QVERIFY2(drive, qPrintable(error));
        const auto storage = ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error); QVERIFY2(storage, qPrintable(error));
        QCOMPARE(storage->drive().identifier(), drive->identifier()); QCOMPARE(storage->drive().storageKind(), SocietyDrive::StorageKind::Package);
        QVERIFY(!QFileInfo::exists(group.filePath("Library/Application Support/iisacc/Logical Disks")));
    }
    void hostIdentityChangesKeepTheRegistryAndManagedOriginalsAttached() {
        QTemporaryDir group(LOGICAL_STORAGE_TEST_DIRECTORY "/host-binding-XXXXXX"); QString error;
        auto storage = ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error); QVERIFY2(storage, qPrintable(error));
        put(storage->filePath(StoreSection::Models, "fixture.safetensors"), "local model");
        put(storage->filePath(StoreSection::GenerationHistory, "generated.png"), "local image");
        QVERIFY2(storage->recordFile(StoreSection::GenerationHistory, "generated.png", &error), qPrintable(error));
        const auto tracking = storage->fileTracking(StoreSection::GenerationHistory, "generated.png", &error);
        const auto reference = storage->models().first().reference(storage->drive().identifier());
        const auto physical = storage->drive().storageIdentifier(), root = storage->drive().rootPath();
        for (int i = 0; i < 3; ++i) {
            const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QVERIFY(SocietyDrive::adoptReplicaIdentity(root, storage->drive().identifier(), host, &error));
            QVERIFY(SocietyDrive::completeReplica(root, host, &error));
            QCOMPARE(ApplicationStorage::selectedRoot(group.path(), &error), root);
            storage = ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error); QVERIFY2(storage, qPrintable(error));
            QCOMPARE(storage->drive().identifier(), host); QCOMPARE(storage->drive().storageIdentifier(), physical);
            QCOMPARE(storage->fileTracking(StoreSection::GenerationHistory, "generated.png", &error), tracking);
            QVERIFY2(!storage->resolveModel(reference, &error).isEmpty(), qPrintable(error));
        }
    }
    void malformedRegistrationAndConflictingFilesNeverCreateAReplacement() {
        QTemporaryDir group(LOGICAL_STORAGE_TEST_DIRECTORY "/invalid-XXXXXX"); QString error;
        QVERIFY(!ApplicationStorage::openInGroup(group.path(), "../outside", &error));
        QVERIFY(QDir().mkpath(group.filePath("Library/Application Support/iisacc/Society")));
        const auto registry = group.filePath("Library/Application Support/iisacc/Society/storage.json"); put(registry, "{invalid");
        QVERIFY(!ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error)); QCOMPARE(get(registry), "{invalid");
        QVERIFY(!QFileInfo::exists(group.filePath("Library/Application Support/iisacc/Logical Disks")));
        QTemporaryDir existing(LOGICAL_STORAGE_TEST_DIRECTORY "/existing-XXXXXX"); put(existing.filePath("keep"), "user data");
        QVERIFY(!SocietyDrive::createLogical(existing.path(), "com.iisacc.dreamscapes", &error)); QCOMPARE(get(existing.filePath("keep")), "user data");
        QVERIFY(!QFileInfo::exists(existing.filePath(".society-logical-disk")));
        const auto packageName = group.filePath("Wrong.SOCIETYCONTAINER");
        QVERIFY(!SocietyDrive::createLogical(packageName, "com.iisacc.dreamscapes", &error)); QVERIFY(!QFileInfo::exists(packageName));
        QTemporaryDir pending(LOGICAL_STORAGE_TEST_DIRECTORY "/partial-logical-XXXXXX");
        put(pending.filePath(".society-logical-disk"), "iisacc.logical-disk.v1\ncom.iisacc.dreamscapes\n");
        QVERIFY(!DirectoryStorage::prepare(pending.path().toStdString()));
        QVERIFY(!QFileInfo::exists(pending.filePath("Society.societycontainer")));
    }
};
QTEST_GUILESS_MAIN(LogicalStorageTests)
#include "logical_storage.moc"
