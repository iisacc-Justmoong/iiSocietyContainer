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
#include "CTestSupport.h"
#include <QUuid>
#include <array>

using namespace iiSocietyContainer;
namespace {
void put(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
    CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write(bytes), bytes.size());
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
public:
    void nativeDesktopRootIsIndependentOfApplicationMetadata() {
#if defined(Q_OS_WIN) || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID))
        QTemporaryDir directory(QDir::current().filePath("suite-native-XXXXXX")); CTEST_CHECK(directory.isValid());
        IsolatedSelection selection(directory.path()); qunsetenv("IISACC_SHARED_STORAGE_ROOT"); QString error;
        const auto expected = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("iisacc/Society");
        CTEST_COMPARE(ApplicationStorage::sharedRoot(&error), expected); CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        QCoreApplication::setApplicationName("Another iisacc product");
        CTEST_COMPARE(ApplicationStorage::sharedRoot(&error), expected);
#else
        society_ctest::skip("Native user data root selection requires Windows or Linux.");
#endif
    }
    void suiteDiscoveryIsIndependentOfTheProductAndReadersDoNotBootstrap() {
        QTemporaryDir directory(QDir::current().filePath("suite-XXXXXX")); CTEST_CHECK(directory.isValid());
        IsolatedSelection selection(directory.path()); QString error;
        const auto root = directory.filePath("suite");
        CTEST_CHECK(!SharedStorage::open({}, &error)); CTEST_CHECK(!QFileInfo::exists(root));
        const auto first = ApplicationStorage::openForApplication("com.iisacc.dreamscapes", &error);
        CTEST_CHECK_MESSAGE(first, qPrintable(error));
        CTEST_COMPARE(ApplicationStorage::sharedRoot(&error), root);
        CTEST_COMPARE(ApplicationStorage::sharedGroupRoot(&error), root);
        const auto model = first->filePath(StoreSection::Models, "shared.safetensors"); put(model, "suite model");
        QCoreApplication::setApplicationName("Another iisacc product");
        const auto reader = SharedStorage::open({}, &error); CTEST_CHECK_MESSAGE(reader, qPrintable(error));
        const auto second = ApplicationStorage::openForApplication("com.iisacc.another", &error); CTEST_CHECK_MESSAGE(second, qPrintable(error));
        CTEST_COMPARE(reader->drive().identifier(), first->drive().identifier());
        CTEST_COMPARE(second->drive().rootPath(), first->drive().rootPath());
        CTEST_COMPARE(second->drive().ownerApplication(), "com.iisacc.dreamscapes");
        CTEST_COMPARE(second->filePath(StoreSection::Models, "shared.safetensors"), model);
        CTEST_COMPARE(get(model), "suite model");
        CTEST_COMPARE(ApplicationStorage::registration(root, &error).value("ownerApplication"), "com.iisacc.dreamscapes");
    }
    void explicitSelectionsAndInvalidSuiteRootsNeverForkStorage() {
        QTemporaryDir directory(QDir::current().filePath("suite-selection-XXXXXX")); CTEST_CHECK(directory.isValid());
        IsolatedSelection selection(directory.path()); QString error;
        const auto drive = SocietyDrive::createAt(directory.path(), &error); CTEST_CHECK_MESSAGE(drive, qPrintable(error));
        qputenv("SOCIETY_CONTAINER_PATH", drive->rootPath().toUtf8());
        const auto existing = ApplicationStorage::openForApplication("com.iisacc.another", &error); CTEST_CHECK_MESSAGE(existing, qPrintable(error));
        CTEST_COMPARE(existing->drive().rootPath(), drive->rootPath()); CTEST_CHECK(!QFileInfo::exists(directory.filePath("suite")));
        qunsetenv("SOCIETY_CONTAINER_PATH");
        put(directory.filePath("selection.json"), "{broken");
        CTEST_CHECK(!ApplicationStorage::openForApplication("com.iisacc.another", &error)); CTEST_CHECK(!error.isEmpty());
        CTEST_CHECK(!QFileInfo::exists(directory.filePath("suite"))); CTEST_CHECK(QFile::remove(directory.filePath("selection.json")));
        for (const auto &invalid : {QByteArray(""), QByteArray("relative-suite")}) {
            qputenv("IISACC_SHARED_STORAGE_ROOT", invalid);
            // Some Windows runtimes remove a variable when assigned an empty
            // value. Never let this negative fixture bootstrap a real default.
            if (!qEnvironmentVariableIsSet("IISACC_SHARED_STORAGE_ROOT")) continue;
            CTEST_CHECK(!ApplicationStorage::openForApplication("com.iisacc.another", &error)); CTEST_CHECK(!error.isEmpty());
            CTEST_CHECK(!QFileInfo::exists(directory.filePath("suite")));
        }
    }
    void redirectedSuiteRootsNeverTouchAnotherDirectory() {
        QTemporaryDir directory(QDir::current().filePath("suite-redirect-XXXXXX")); CTEST_CHECK(directory.isValid());
        IsolatedSelection selection(directory.path()); QString error;
        const auto outside = directory.filePath("outside"); CTEST_CHECK(QDir().mkdir(outside));
        std::error_code linkError;
        std::filesystem::create_directory_symlink(std::filesystem::path(outside.toStdU16String()),
            std::filesystem::path(directory.filePath("suite").toStdU16String()), linkError);
        if (linkError) society_ctest::skip("This environment does not permit directory symlinks.");
        CTEST_CHECK(!ApplicationStorage::openForApplication("com.iisacc.another", &error)); CTEST_CHECK(!error.isEmpty());
        CTEST_CHECK(QDir(outside).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    }
    void sharedGuardIsPrivateAndRejectsRedirection() {
#if defined(Q_OS_DARWIN)
        QTemporaryDir group(QDir::current().filePath("guard-XXXXXX")); CTEST_CHECK(group.isValid());
        QTemporaryDir outside(QDir::current().filePath("guard-outside-XXXXXX")); CTEST_CHECK(outside.isValid());
        const auto sentinel = outside.filePath("sentinel"); put(sentinel, "unchanged");
        const auto guard = group.filePath(".iisacc-storage.guard");
        CTEST_CHECK(QFile::link(sentinel, guard)); QString error;
        CTEST_CHECK(!ApplicationStorage::openInGroup(group.path(), "com.iisacc.guard-test", &error));
        CTEST_CHECK(!error.isEmpty()); CTEST_COMPARE(get(sentinel), QByteArray("unchanged"));
        CTEST_CHECK(QFile::remove(guard));
        CTEST_CHECK_MESSAGE(ApplicationStorage::openInGroup(group.path(), "com.iisacc.guard-test", &error), qPrintable(error));
        const auto permissions = QFile::permissions(guard);
        CTEST_CHECK(!(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)));
        CTEST_COMPARE(QFileInfo(guard).size(), qint64(0)); CTEST_COMPARE(get(sentinel), QByteArray("unchanged"));
#else
        society_ctest::skip("The shared Apple ownership guard is platform-specific.");
#endif
    }
    void applicationOwnsPersistentLogicalDiskWithoutAPackageOrSociety() {
        QTemporaryDir group(QDir::current().filePath("group-XXXXXX")); CTEST_CHECK(group.isValid()); QString error;
        const auto storage = ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error);
        CTEST_CHECK_MESSAGE(storage, qPrintable(error)); const auto drive = storage->drive();
        CTEST_COMPARE(drive.storageKind(), SocietyDrive::StorageKind::LogicalDisk);
        CTEST_COMPARE(drive.ownerApplication(), "com.iisacc.dreamscapes");
        CTEST_CHECK(!drive.rootPath().endsWith(".societycontainer")); CTEST_CHECK(!QFileInfo::exists(group.filePath("Society.societycontainer")));
        CTEST_CHECK(drive.isReady()); CTEST_COMPARE(drive.sections().size(), 9);
        const auto reused = SocietyDrive::createAt(drive.rootPath(), &error); CTEST_CHECK_MESSAGE(reused, qPrintable(error));
        CTEST_COMPARE(reused->identifier(), drive.identifier());
        const auto model = storage->filePath(StoreSection::Models, "fixture.safetensors", &error); CTEST_CHECK_MESSAGE(!model.isEmpty(), qPrintable(error));
        put(model, "model download bytes"); const auto image = storage->filePath(StoreSection::GenerationHistory, "generated.png", &error);
        put(image, "generated image bytes"); const auto identifier = drive.identifier();
        const auto reopened = ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error);
        CTEST_CHECK_MESSAGE(reopened, qPrintable(error)); CTEST_COMPARE(reopened->drive().identifier(), identifier);
        CTEST_COMPARE(get(model), "model download bytes"); CTEST_COMPARE(get(image), "generated image bytes");
        CTEST_COMPARE(ApplicationStorage::registration(group.path(), &error).value("managingApplication"), "com.iisacc.dreamscapes");
    }
    void societyAdoptsTheSameDiskAndPreservesPathsIdentityReferencesAndManagedHistory() {
        QTemporaryDir group(QDir::current().filePath("adoption-XXXXXX")); QString error;
        const auto before = ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error); CTEST_CHECK_MESSAGE(before, qPrintable(error));
        const auto model = before->filePath(StoreSection::Models, "fixture.safetensors"); put(model, "original model");
        const auto image = before->filePath(StoreSection::GenerationHistory, "generated.png"); put(image, "generated image");
        CTEST_CHECK_MESSAGE(before->recordFile(StoreSection::GenerationHistory, "generated.png", &error), qPrintable(error));
        const auto tracking = before->fileTracking(StoreSection::GenerationHistory, "generated.png", &error); CTEST_CHECK_MESSAGE(!tracking.isEmpty(), qPrintable(error));
        const auto reference = before->models().first().reference(before->drive().identifier());
        const auto adopted = ApplicationStorage::adoptInGroup(group.path(), &error); CTEST_CHECK_MESSAGE(adopted, qPrintable(error));
        CTEST_COMPARE(adopted->drive().rootPath(), before->drive().rootPath()); CTEST_COMPARE(adopted->drive().identifier(), before->drive().identifier());
        CTEST_CHECK(before->drive().isReady()); CTEST_COMPARE(adopted->resolveModel(reference, &error), model);
        CTEST_COMPARE(adopted->fileTracking(StoreSection::GenerationHistory, "generated.png", &error), tracking);
        CTEST_COMPARE(get(image), "generated image");
        CTEST_COMPARE(ApplicationStorage::registration(group.path(), &error).value("managingApplication"), "com.iisacc.society");
        const auto after = ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error); CTEST_CHECK(after);
        CTEST_COMPARE(after->drive().identifier(), before->drive().identifier());
        CTEST_COMPARE(ApplicationStorage::registration(group.path(), &error).value("managingApplication"), "com.iisacc.society");
    }
    void existingPackageWinsWithoutCreatingAnotherDisk() {
        QTemporaryDir group(QDir::current().filePath("package-XXXXXX")); QString error;
        const auto drive = SocietyDrive::createAt(group.path(), &error); CTEST_CHECK_MESSAGE(drive, qPrintable(error));
        const auto storage = ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error); CTEST_CHECK_MESSAGE(storage, qPrintable(error));
        CTEST_COMPARE(storage->drive().identifier(), drive->identifier()); CTEST_COMPARE(storage->drive().storageKind(), SocietyDrive::StorageKind::Package);
        CTEST_CHECK(!QFileInfo::exists(group.filePath("Library/Application Support/iisacc/Logical Disks")));
    }
    void hostIdentityChangesKeepTheRegistryAndManagedOriginalsAttached() {
        QTemporaryDir group(QDir::current().filePath("host-binding-XXXXXX")); QString error;
        auto storage = ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error); CTEST_CHECK_MESSAGE(storage, qPrintable(error));
        put(storage->filePath(StoreSection::Models, "fixture.safetensors"), "local model");
        put(storage->filePath(StoreSection::GenerationHistory, "generated.png"), "local image");
        CTEST_CHECK_MESSAGE(storage->recordFile(StoreSection::GenerationHistory, "generated.png", &error), qPrintable(error));
        const auto tracking = storage->fileTracking(StoreSection::GenerationHistory, "generated.png", &error);
        const auto reference = storage->models().first().reference(storage->drive().identifier());
        const auto physical = storage->drive().storageIdentifier(), root = storage->drive().rootPath();
        for (int i = 0; i < 3; ++i) {
            const auto host = QUuid::createUuid().toString(QUuid::WithoutBraces);
            CTEST_CHECK(SocietyDrive::adoptReplicaIdentity(root, storage->drive().identifier(), host, &error));
            CTEST_CHECK(SocietyDrive::completeReplica(root, host, &error));
            CTEST_COMPARE(ApplicationStorage::selectedRoot(group.path(), &error), root);
            storage = ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error); CTEST_CHECK_MESSAGE(storage, qPrintable(error));
            CTEST_COMPARE(storage->drive().identifier(), host); CTEST_COMPARE(storage->drive().storageIdentifier(), physical);
            CTEST_COMPARE(storage->fileTracking(StoreSection::GenerationHistory, "generated.png", &error), tracking);
            CTEST_CHECK_MESSAGE(!storage->resolveModel(reference, &error).isEmpty(), qPrintable(error));
        }
    }
    void malformedRegistrationAndConflictingFilesNeverCreateAReplacement() {
        QTemporaryDir group(QDir::current().filePath("invalid-XXXXXX")); QString error;
        CTEST_CHECK(!ApplicationStorage::openInGroup(group.path(), "../outside", &error));
        CTEST_CHECK(QDir().mkpath(group.filePath("Library/Application Support/iisacc/Society")));
        const auto registry = group.filePath("Library/Application Support/iisacc/Society/storage.json"); put(registry, "{invalid");
        CTEST_CHECK(!ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error)); CTEST_COMPARE(get(registry), "{invalid");
        CTEST_CHECK(!QFileInfo::exists(group.filePath("Library/Application Support/iisacc/Logical Disks")));
        QTemporaryDir existing(QDir::current().filePath("existing-XXXXXX")); put(existing.filePath("keep"), "user data");
        CTEST_CHECK(!SocietyDrive::createLogical(existing.path(), "com.iisacc.dreamscapes", &error)); CTEST_COMPARE(get(existing.filePath("keep")), "user data");
        CTEST_CHECK(!QFileInfo::exists(existing.filePath(".society-logical-disk")));
        const auto packageName = group.filePath("Wrong.SOCIETYCONTAINER");
        CTEST_CHECK(!SocietyDrive::createLogical(packageName, "com.iisacc.dreamscapes", &error)); CTEST_CHECK(!QFileInfo::exists(packageName));
        QTemporaryDir pending(QDir::current().filePath("partial-logical-XXXXXX"));
        put(pending.filePath(".society-logical-disk"), "iisacc.logical-disk.v1\ncom.iisacc.dreamscapes\n");
        CTEST_CHECK(!DirectoryStorage::prepare(pending.path().toStdString()));
        CTEST_CHECK(!QFileInfo::exists(pending.filePath("Society.societycontainer")));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name" << std::endl; return 1; }
    society_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    LogicalStorageTests tests;
    runner.run("nativeDesktopRootIsIndependentOfApplicationMetadata", [&] { tests.nativeDesktopRootIsIndependentOfApplicationMetadata(); });
    runner.run("suiteDiscoveryIsIndependentOfTheProductAndReadersDoNotBootstrap", [&] { tests.suiteDiscoveryIsIndependentOfTheProductAndReadersDoNotBootstrap(); });
    runner.run("explicitSelectionsAndInvalidSuiteRootsNeverForkStorage", [&] { tests.explicitSelectionsAndInvalidSuiteRootsNeverForkStorage(); });
    runner.run("redirectedSuiteRootsNeverTouchAnotherDirectory", [&] { tests.redirectedSuiteRootsNeverTouchAnotherDirectory(); });
    runner.run("sharedGuardIsPrivateAndRejectsRedirection", [&] { tests.sharedGuardIsPrivateAndRejectsRedirection(); });
    runner.run("applicationOwnsPersistentLogicalDiskWithoutAPackageOrSociety", [&] { tests.applicationOwnsPersistentLogicalDiskWithoutAPackageOrSociety(); });
    runner.run("societyAdoptsTheSameDiskAndPreservesPathsIdentityReferencesAndManagedHistory", [&] { tests.societyAdoptsTheSameDiskAndPreservesPathsIdentityReferencesAndManagedHistory(); });
    runner.run("existingPackageWinsWithoutCreatingAnotherDisk", [&] { tests.existingPackageWinsWithoutCreatingAnotherDisk(); });
    runner.run("hostIdentityChangesKeepTheRegistryAndManagedOriginalsAttached", [&] { tests.hostIdentityChangesKeepTheRegistryAndManagedOriginalsAttached(); });
    runner.run("malformedRegistrationAndConflictingFilesNeverCreateAReplacement", [&] { tests.malformedRegistrationAndConflictingFilesNeverCreateAReplacement(); });
    return runner.result();
}
