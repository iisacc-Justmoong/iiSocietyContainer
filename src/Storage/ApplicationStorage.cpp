#include "ApplicationStorage.h"
#include "LogicalDisk.h"
#include <iiFileProviderDevice.h>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QUuid>

namespace iiSocietyContainer {
#ifdef Q_OS_DARWIN
QString appleSharedApplicationGroupRoot(QString *error);
bool appleProtectLogicalDisk(const QString &path, QString *error);
int appleAcquireStorageRegistrationLock(const QString &group, QString *error);
void appleReleaseStorageRegistrationLock(int descriptor);
#endif
namespace {
class PlatformGroupLock final {
public:
    PlatformGroupLock(const QString &group, QString *error) {
#ifdef Q_OS_DARWIN
        m_descriptor = appleAcquireStorageRegistrationLock(group, error);
#else
        Q_UNUSED(group); Q_UNUSED(error);
#endif
    }
    ~PlatformGroupLock() {
#ifdef Q_OS_DARWIN
        if (m_descriptor >= 0) appleReleaseStorageRegistrationLock(m_descriptor);
#endif
    }
    explicit operator bool() const { return m_descriptor >= 0; }
private:
    int m_descriptor = 0;
};
const QString registryRelative = "Library/Application Support/iisacc/Society/storage.json";
void fail(QString *error, const QString &message) { if (error) *error = message; }
bool present(const QString &path) { const QFileInfo info(path); return info.exists() || info.isSymLink() || info.isJunction(); }
QString sharedDirectory(const QString &path, bool create, QString *error) {
    if (path.isEmpty() || !QFileInfo(path).isAbsolute() || path.contains(QChar::Null)
        || iiFileProvider::Directory::cleanPath(path) != path) {
        fail(error, "The iisacc shared storage root must be an absolute canonical directory."); return {};
    }
    QStringList missing; auto ancestor = path;
    while (!present(ancestor)) {
        const QFileInfo info(ancestor); const auto parent = info.absolutePath();
        if (parent == ancestor) { fail(error, "The shared storage parent is unavailable."); return {}; }
        missing.prepend(info.fileName()); ancestor = parent;
    }
    const auto valid = [](const QString &candidate) {
        const QFileInfo info(candidate);
        return info.isDir() && !info.isSymLink() && !info.isJunction() && info.canonicalFilePath() == candidate;
    };
    if (!valid(ancestor)) { fail(error, "The shared storage root or parent is redirected or is not a directory."); return {}; }
    if (create) for (const auto &part : missing) {
        ancestor = iiFileProvider::Directory(ancestor).filePath(part);
        if ((!iiFileProvider::Directory().mkdir(ancestor) && !present(ancestor)) || !valid(ancestor)) {
            fail(error, "The iisacc shared storage directory could not be prepared safely."); return {};
        }
    }
    return path;
}
QString checkedGroup(const QString &path, QString *error) {
    const QFileInfo info(path);
    if (!info.isAbsolute() || !info.isDir() || info.isSymLink() || info.isJunction() || info.canonicalFilePath() != path) {
        fail(error, "The shared application group must be an existing canonical directory."); return {};
    }
#ifdef Q_OS_IOS
    const auto entitled = appleSharedApplicationGroupRoot(error);
    if (entitled.isEmpty() || (path != entitled && !path.startsWith(entitled + '/'))) {
        fail(error, "Application-owned storage must stay inside the entitled shared App Group."); return {};
    }
#endif
    return path;
}
QString confined(const QString &group, const QString &relative, bool createDirectories, QString *error) {
    if (relative.isEmpty() || relative.startsWith('/') || relative.contains('\\') || relative.contains(':') || relative.contains(QChar::Null)) {
        fail(error, "Invalid shared storage path."); return {};
    }
    auto current = group;
    for (const auto &part : relative.split('/')) {
        if (part.isEmpty() || part == "." || part == "..") { fail(error, "Invalid shared storage path."); return {}; }
        current = iiFileProvider::Directory(current).filePath(part);
        if (createDirectories && !present(current) && !iiFileProvider::Directory().mkdir(current)) {
            fail(error, "The shared storage directory could not be created."); return {};
        }
        const QFileInfo info(current);
        if (present(current) && (info.isSymLink() || info.isJunction() || info.canonicalFilePath() != current
            || (createDirectories && !info.isDir()))) { fail(error, "The shared storage path is redirected or has conflicting contents."); return {}; }
    }
    return current;
}
QString registryPath(const QString &group, QString *error) { return confined(group, registryRelative, false, error); }
bool saveRegistration(const QString &group, const SocietyDrive &drive, const QString &creator, const QString &manager, QString *error) {
    if (confined(group, "Library/Application Support/iisacc/Society", true, error).isEmpty()) return false;
    const auto path = registryPath(group, error); if (path.isEmpty()) return false;
    const auto relative = iiFileProvider::Directory(group).relativeFilePath(drive.rootPath());
    if (confined(group, relative, false, error) != drive.rootPath()) return false;
    const auto storageId = drive.storageIdentifier();
    if (QUuid(storageId).isNull()) { fail(error, "The shared disk identity changed before registration."); return false; }
    const QJsonObject record{{"schemaVersion", 1}, {"path", relative}, {"containerId", drive.identifier()},
        {"storageId", storageId},
        {"ownerApplication", creator}, {"managingApplication", manager},
        {"storageKind", drive.storageKind() == SocietyDrive::StorageKind::LogicalDisk ? "logical" : "package"}};
    iiFileProvider::SaveFile output(path); const auto bytes = QJsonDocument(record).toJson();
    if (!output.open(QIODevice::WriteOnly) || !output.setPermissions(iiFileProvider::FileDevice::ReadOwner | iiFileProvider::FileDevice::WriteOwner)
        || output.write(bytes) != bytes.size() || !output.commit()) { fail(error, "The shared storage registration could not be committed."); return false; }
    return true;
}
}
#ifdef Q_OS_IOS
QString iosSharedStorageRoot(QString *error) {
    const auto group = appleSharedApplicationGroupRoot(error);
    return group.isEmpty() ? QString{} : ApplicationStorage::selectedRoot(group, error);
}
#endif
QString ApplicationStorage::sharedRoot(QString *error) {
    if (error) error->clear();
#if !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID) && !defined(Q_OS_WASM)
    if (qEnvironmentVariableIsSet("IISACC_SHARED_STORAGE_ROOT"))
        return sharedDirectory(qEnvironmentVariable("IISACC_SHARED_STORAGE_ROOT"), false, error);
#endif
#ifdef Q_OS_DARWIN
    return appleSharedApplicationGroupRoot(error);
#elif defined(Q_OS_ANDROID)
    fail(error, "Android products access the common Society container through the signed storage provider."); return {};
#elif defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    const auto data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (data.isEmpty()) { fail(error, "The operating system did not provide a shared user data location."); return {}; }
    return sharedDirectory(iiFileProvider::Directory(data).filePath("iisacc/Society"), false, error);
#else
    fail(error, "Configure a shared storage provider for this platform."); return {};
#endif
}
QString ApplicationStorage::sharedGroupRoot(QString *error) { return sharedRoot(error); }
QJsonObject ApplicationStorage::registration(const QString &sharedGroup, QString *error) {
    if (error) error->clear(); const auto group = checkedGroup(sharedGroup, error); if (group.isEmpty()) return {};
    const auto path = registryPath(group, error); if (path.isEmpty() || !present(path)) return {};
    const QFileInfo info(path); iiFileProvider::FileDevice file(path);
    if (!info.isFile() || info.size() <= 0 || info.size() > 65536 || !file.open(QIODevice::ReadOnly)) { fail(error, "The shared storage registration is unreadable."); return {}; }
    QJsonParseError parse; const auto document = QJsonDocument::fromJson(file.readAll(), &parse); const auto record = document.object();
    if (parse.error != QJsonParseError::NoError || record.value("schemaVersion") != 1
        || !LogicalDisk::validOwner(record.value("ownerApplication").toString().toStdString())
        || !LogicalDisk::validOwner(record.value("managingApplication").toString().toStdString())
        || (record.value("storageKind") != "logical" && record.value("storageKind") != "package")
        || QUuid(record.value("containerId").toString()).isNull() || QUuid(record.value("storageId").toString()).isNull()
        || record.value("path").toString().isEmpty()) {
        fail(error, "The shared storage registration is invalid or unsupported."); return {};
    }
    return record;
}
QString ApplicationStorage::selectedRoot(const QString &sharedGroup, QString *error) {
    if (error) error->clear(); const auto group = checkedGroup(sharedGroup, error); if (group.isEmpty()) return {};
    const auto path = registryPath(group, error); if (path.isEmpty()) return {};
    if (present(path)) {
        const auto record = registration(group, error); if (record.isEmpty()) return {};
        const auto selected = confined(group, record.value("path").toString(), false, error); if (selected.isEmpty()) return {};
        const auto drive = SocietyDrive::open(selected, error); if (!drive) return {};
        if (drive->storageIdentifier() != record.value("storageId")
            || (drive->storageKind() == SocietyDrive::StorageKind::LogicalDisk) != (record.value("storageKind") == "logical")
            || (drive->storageKind() == SocietyDrive::StorageKind::LogicalDisk && drive->ownerApplication() != record.value("ownerApplication"))) {
            fail(error, "The registered shared disk identity or owner changed."); return {};
        }
        return drive->rootPath();
    }
    const auto package = confined(group, "Society.societycontainer", false, error); if (package.isEmpty()) return {};
    if (present(package)) { const auto drive = SocietyDrive::open(package, error); return drive ? drive->rootPath() : QString{}; }
    fail(error, "No shared Society storage has been prepared."); return {};
}
std::optional<SharedStorage> ApplicationStorage::openForApplication(const QString &application, QString *error) {
    if (error) error->clear();
    if (!LogicalDisk::validOwner(application.toStdString())) { fail(error, "Use a valid application identifier."); return {}; }
    QString selectedError; if (auto existing = SharedStorage::open({}, &selectedError)) return existing;
    if (!qEnvironmentVariableIsEmpty("SOCIETY_CONTAINER_PATH")) { fail(error, selectedError); return {}; }
#if !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
    if (SharedStorage::settingsPath().isEmpty() || present(SharedStorage::settingsPath())) { fail(error, selectedError); return {}; }
#endif
    const auto root = sharedRoot(error); if (root.isEmpty()) return {};
    const auto group = sharedDirectory(root, true, error);
    return group.isEmpty() ? std::nullopt : openInGroup(group, application, error);
}
std::optional<SharedStorage> ApplicationStorage::openInGroup(const QString &sharedGroup, const QString &application, QString *error) {
    if (error) error->clear();
    if (!LogicalDisk::validOwner(application.toStdString())) { fail(error, "Use a valid application identifier."); return {}; }
    const auto group = checkedGroup(sharedGroup, error); if (group.isEmpty()) return {};
    PlatformGroupLock platformLock(group, error); if (!platformLock) return {};
    iiFileProvider::LockFile lock(iiFileProvider::Directory(group).filePath(".iisacc-storage.lock")); lock.setStaleLockTime(0);
    if (!lock.tryLock(5000)) { fail(error, "Another application is preparing shared storage."); return {}; }
    const auto registry = registryPath(group, error); if (registry.isEmpty()) return {};
    const auto package = confined(group, "Society.societycontainer", false, error); if (package.isEmpty()) return {};
    if (present(registry) || present(package)) {
        const auto selected = selectedRoot(group, error); return selected.isEmpty() ? std::nullopt : SharedStorage::open(selected, error);
    }
    const auto parent = confined(group, "Library/Application Support/iisacc/Logical Disks", true, error); if (parent.isEmpty()) return {};
    const auto root = iiFileProvider::Directory(parent).filePath(application);
    const auto drive = SocietyDrive::createLogical(root, application, error); if (!drive) return {};
#ifdef Q_OS_DARWIN
    if (!appleProtectLogicalDisk(root, error)) return {};
#endif
    if (!saveRegistration(group, *drive, application, application, error)) return {};
    return SharedStorage::open(root, error);
}
std::optional<SharedStorage> ApplicationStorage::adoptInGroup(const QString &sharedGroup, QString *error) {
    if (error) error->clear(); const auto group = checkedGroup(sharedGroup, error); if (group.isEmpty()) return {};
    PlatformGroupLock platformLock(group, error); if (!platformLock) return {};
    iiFileProvider::LockFile lock(iiFileProvider::Directory(group).filePath(".iisacc-storage.lock")); lock.setStaleLockTime(0);
    if (!lock.tryLock(5000)) { fail(error, "Another application is updating shared storage ownership."); return {}; }
    const auto selected = selectedRoot(group, error); if (selected.isEmpty()) return {};
    const auto drive = SocietyDrive::open(selected, error); if (!drive) return {};
    const auto creator = drive->ownerApplication().isEmpty() ? QString("com.iisacc.society") : drive->ownerApplication();
    if (!saveRegistration(group, *drive, creator, "com.iisacc.society", error)) return {};
    return SharedStorage::open(selected, error);
}
}
