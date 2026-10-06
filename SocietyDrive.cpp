#include "SocietyDrive.h"
#include "src/FileTreePaths.h"
#include "ModelLayout.h"
#include "FilesLayout.h"
#include "PhotosLayout.h"
#include "DiskImage.h"
#include "DirectoryStorage.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QUuid>

#include <utility>

namespace iiSocietyContainer {
namespace {

constexpr auto manifestName = ".society-drive.json";

QString sectionRoot(const QString& root, StoreSection section)
{
    if (section == StoreSection::Files && QFileInfo::exists(root + "/.society-disk.plist")) {
        const auto files = DiskImage::filesRoot(root.toStdString());
        return files ? QString::fromStdString(files->string()) : QString();
    }
    return QDir(root).filePath(storeSectionName(section));
}

void setError(QString* error, const QString& message)
{
    if (error) {
        *error = message;
    }
}

bool isDirectDirectory(const QString& path)
{
    const QFileInfo info(path);
    return info.isDir() && !info.isSymLink() && !info.isJunction()
        && info.canonicalFilePath() == path;
}

QString sourceLocationError(const QString& path)
{
    if (const auto container = DiskImage::containerRoot(path.toStdString()))
        return QStringLiteral("Society Files is a public volume. Open the original Society disk image in the app.");
#ifdef Q_OS_MACOS
    const QFileInfo cloudStorage(QDir::home().filePath(QStringLiteral("Library/CloudStorage")));
    const auto cloudRoot = cloudStorage.canonicalFilePath();
    if (!cloudRoot.isEmpty() && (path == cloudRoot || path.startsWith(cloudRoot + '/'))) {
        return QStringLiteral("Finder's CloudStorage drive is a public replica. Choose the original Society source folder.");
    }
#endif
    // A Files replica was previously accepted as a new eight-section drive.
    // Also reject the corresponding native Files path and deeper descendants.
    QDir ancestor(path);
    while (ancestor.cdUp()) {
        const QFileInfo manifest(ancestor.filePath(QLatin1String(manifestName)));
        if (manifest.exists() || manifest.isSymLink() || manifest.isJunction()) {
            return QStringLiteral("A Society source cannot be inside another container. Choose its original source folder.");
        }
    }
    return {};
}

QJsonArray sectionDefinitions(bool legacy = false)
{
    QJsonArray definitions;
    for (const auto section : allStoreSections()) {
        if (legacy && section == StoreSection::Photos) continue;
        definitions.append(QJsonObject{
            {"id", storeSectionKey(section)},
            {"name", storeSectionName(section)},
            {"path", storeSectionName(section)}
        });
    }
    return definitions;
}

} // namespace

SocietyDrive::SocietyDrive(QString root, QString identifier)
    : m_rootPath(std::move(root)), m_identifier(std::move(identifier))
{
}

std::optional<SocietyDrive> SocietyDrive::createAt(const QString& parentDirectory, QString* error)
{
    setError(error, {});
    const auto location = DirectoryStorage::prepare(detail::nativePath(parentDirectory));
    if (!location) { setError(error, QString::fromStdString(location.error())); return {}; }
#ifdef Q_OS_WIN
    const auto directory = QString::fromStdWString(location->path.wstring());
#else
    const auto directory = QString::fromStdString(location->path.string());
#endif
    if (const auto message = sourceLocationError(directory); !message.isEmpty()) {
        setError(error, message);
        if (location->created) { std::error_code cleanup; std::filesystem::remove(location->path, cleanup); }
        return {};
    }
    auto drive = create(directory, error);
    if (!drive && location->created) {
        std::error_code cleanup;
        std::filesystem::remove(location->path, cleanup); // Empty directories only.
    }
    return drive;
}

std::optional<SocietyDrive> SocietyDrive::create(const QString& directoryPath, QString* error)
{
    setError(error, {});
    const SocietyContainer container(directoryPath);
    if (!container.isValid()) {
        setError(error, container.errorString());
        return std::nullopt;
    }

    if (const auto locationError = sourceLocationError(container.rootPath()); !locationError.isEmpty()) {
        setError(error, locationError);
        return std::nullopt;
    }

    const QDir root(container.rootPath());
    QLockFile lock(root.filePath(QStringLiteral(".society-drive.lock")));
    if (!lock.tryLock()) {
        setError(error, QStringLiteral("The drive is being initialized by another process."));
        return std::nullopt;
    }
    const QFileInfo manifest(root.filePath(QLatin1String(manifestName)));
    if (manifest.exists() || manifest.isSymLink()) {
        const auto drive = open(root.path(), error);
        if (!drive) return {};
        if (!drive->isReady()) return drive;
        const auto modelsRoot = drive->sectionPath(StoreSection::Models);
        QLockFile modelsLock(QDir(modelsRoot).filePath(".society-models.lock"));
        if (!modelsLock.tryLock(5000)) {
            setError(error, QStringLiteral("Another process is organizing Models.")); return {};
        }
        if (!drive->isReady() || !detail::createModelLayout(modelsRoot, error)) return {};
        return drive;
    }

    for (const auto section : allStoreSections()) {
        const QString path = sectionRoot(root.path(), section);
        if (path.isEmpty()) { setError(error, "The Society Files volume is unavailable."); return {}; }
        const QFileInfo info(path);
        if ((info.exists() || info.isSymLink() || info.isJunction()) && !isDirectDirectory(path)) {
            setError(error, QStringLiteral("The drive section conflicts with an existing entry: %1").arg(path));
            return std::nullopt;
        }
    }

    QStringList createdDirectories;
    const auto rollback = [&createdDirectories] {
        for (auto it = createdDirectories.crbegin(); it != createdDirectories.crend(); ++it) {
            QDir().rmdir(*it); // Only removes empty directories created by this call.
        }
    };
    const auto modelsRoot = root.filePath(storeSectionName(StoreSection::Models));
    if (!detail::validateModelLayout(modelsRoot, error)) return {};
    const auto filesRoot = sectionRoot(root.path(), StoreSection::Files);
    for (const auto section : allStoreSections()) {
        const QString path = sectionRoot(root.path(), section);
        if (!QFileInfo::exists(path)) {
            if (!QDir().mkdir(path)) {
                rollback();
                setError(error, QStringLiteral("The drive section could not be created: %1").arg(path));
                return std::nullopt;
            }
            createdDirectories.append(path);
        }
    }
    if (!detail::createModelLayout(modelsRoot, error, &createdDirectories)) { rollback(); return {}; }
    if (!detail::migratePhotosLayout(root.path(), error, filesRoot)) { rollback(); return {}; }

    const QString identifier = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonDocument document(QJsonObject{
        {"type", "SocietyDrive"},
        {"schemaVersion", 1},
        {"filesLayoutVersion", 1},
        {"identifier", identifier},
        {"displayName", "Society"},
        {"sections", sectionDefinitions()}
    });
    const QByteArray data = document.toJson();
    QSaveFile output(manifest.filePath());
    if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() || !output.commit()) {
        rollback();
        setError(error, QStringLiteral("The drive manifest could not be saved: %1").arg(output.errorString()));
        return std::nullopt;
    }
    return SocietyDrive(root.path(), identifier);
}

std::optional<SocietyDrive> SocietyDrive::open(const QString& directoryPath, QString* error)
{
    setError(error, {});
    const SocietyContainer container(directoryPath);
    if (!container.isValid()) {
        setError(error, container.errorString());
        return std::nullopt;
    }

    if (const auto locationError = sourceLocationError(container.rootPath()); !locationError.isEmpty()) {
        setError(error, locationError);
        return std::nullopt;
    }

    const QDir root(container.rootPath());
    const QFileInfo manifest(root.filePath(QLatin1String(manifestName)));
    QFile input(manifest.filePath());
    if (!manifest.isFile() || manifest.isSymLink() || manifest.size() > 65536
        || !input.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("The directory has no readable Society drive manifest."));
        return std::nullopt;
    }
    QJsonParseError parseError;
    const auto manifestBytes = input.readAll(); input.close();
    const QJsonDocument document = QJsonDocument::fromJson(manifestBytes, &parseError);
    QJsonObject data = document.object();
    const bool legacyPhotos = data.value("sections") == QJsonValue(sectionDefinitions(true));
    const QString identifier = data.value("identifier").toString();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || data.value("type") != QJsonValue("SocietyDrive")
        || data.value("schemaVersion") != QJsonValue(1)
        || QUuid(identifier).isNull()
        || (data.contains("localIdentifier") && (QUuid(data.value("localIdentifier").toString()).isNull() || !data.value("replicaReady").isBool()))
        || (data.value("displayName") != QJsonValue("Society")
            && data.value("displayName") != QJsonValue("Society Container"))
        || (!legacyPhotos && data.value("sections") != QJsonValue(sectionDefinitions()))) {
        setError(error, QStringLiteral("The Society drive manifest is invalid or unsupported."));
        return std::nullopt;
    }
    for (const auto section : allStoreSections()) {
        if (legacyPhotos && section == StoreSection::Photos) continue;
        const QString path = sectionRoot(root.path(), section);
        if (!isDirectDirectory(path)) {
            setError(error, QStringLiteral("The drive section is missing or redirected: %1").arg(path));
            return std::nullopt;
        }
    }
    const bool legacyFiles = !data.contains("filesLayoutVersion") && data.value("replicaReady") != QJsonValue(false);
    if (legacyPhotos || legacyFiles) {
        QLockFile layoutLock(root.filePath(".society-layout.lock"));
        if (!layoutLock.tryLock(5000)) {
            setError(error, QStringLiteral("Another process is upgrading the Society layout.")); return {};
        }
        if (!input.open(QIODevice::ReadOnly)) { setError(error, input.errorString()); return {}; }
        const auto current = input.readAll(); input.close();
        if (current != manifestBytes) { layoutLock.unlock(); return open(directoryPath, error); }
        if (legacyPhotos) {
            if (!detail::migratePhotosLayout(root.path(), error, sectionRoot(root.path(), StoreSection::Files))) return {};
            data.insert("sections", sectionDefinitions());
        }
        if (legacyFiles) {
            if (!detail::removeLegacyFilesDirectories(sectionRoot(root.path(), StoreSection::Files), error)) return {};
            data.insert("filesLayoutVersion", 1);
        }
        const auto bytes = QJsonDocument(data).toJson();
        QSaveFile output(manifest.filePath());
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
            setError(error, QStringLiteral("Could not save the upgraded Society manifest: %1").arg(output.errorString())); return {};
        }
    }
    return SocietyDrive(root.path(), identifier);
}

std::optional<SocietyDrive> SocietyDrive::adoptReplicaIdentity(
    const QString& directoryPath, const QString& expectedIdentifier,
    const QString& hostIdentifier, QString* error)
{
    setError(error, {});
    if (QUuid(hostIdentifier).isNull()
        || QUuid(hostIdentifier).toString(QUuid::WithoutBraces) != hostIdentifier) {
        setError(error, QStringLiteral("The host drive identifier is invalid."));
        return std::nullopt;
    }
    const auto current = open(directoryPath, error);
    if (!current) return std::nullopt;
    const QDir root(current->rootPath());
    QLockFile lock(root.filePath(QStringLiteral(".society-drive.lock")));
    if (!lock.tryLock()) {
        setError(error, QStringLiteral("The drive is being changed by another process."));
        return std::nullopt;
    }
    const auto checked = open(root.path(), error);
    if (!checked) return std::nullopt;
    if (checked->identifier() != expectedIdentifier && checked->identifier() != hostIdentifier) {
        setError(error, QStringLiteral("The drive identity changed before adoption."));
        return std::nullopt;
    }
    QFile input(root.filePath(QLatin1String(manifestName)));
    if (!input.open(QIODevice::ReadOnly)) {
        setError(error, input.errorString()); return std::nullopt;
    }
    auto manifest = QJsonDocument::fromJson(input.readAll()).object();
    input.close();
    manifest.insert(QStringLiteral("identifier"), hostIdentifier);
    // Native provider domains keep one stable device-local registration while
    // the logical identifier follows the host on every client.
    if (!manifest.contains("localIdentifier")) manifest.insert("localIdentifier", checked->identifier());
    if (checked->identifier() != hostIdentifier || !manifest.contains("previousIdentifier"))
        manifest.insert("previousIdentifier", checked->identifier());
    manifest.insert("replicaReady", false);
    const auto bytes = QJsonDocument(manifest).toJson();
    QSaveFile output(input.fileName());
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
        setError(error, output.errorString()); return std::nullopt;
    }
    return open(root.path(), error);
}

bool SocietyDrive::completeReplica(const QString& directoryPath, const QString& expectedIdentifier, QString* error)
{
    const auto drive = open(directoryPath, error);
    if (!drive) return false;
    const QDir root(drive->rootPath());
    QLockFile lock(root.filePath(QStringLiteral(".society-drive.lock")));
    if (!lock.tryLock()) { setError(error, QStringLiteral("The drive is being changed by another process.")); return false; }
    const auto checked = open(root.path(), error);
    if (!checked || checked->identifier() != expectedIdentifier) {
        setError(error, QStringLiteral("The replica identity changed.")); return false;
    }
    QFile input(root.filePath(QLatin1String(manifestName)));
    if (!input.open(QIODevice::ReadOnly)) { setError(error, input.errorString()); return false; }
    auto manifest = QJsonDocument::fromJson(input.readAll()).object(); input.close();
    if (QUuid(manifest.value("localIdentifier").toString()).isNull()) {
        setError(error, QStringLiteral("The drive is not a replica.")); return false;
    }
    if (!manifest.contains("filesLayoutVersion")) {
        if (!detail::removeLegacyFilesDirectories(sectionRoot(root.path(), StoreSection::Files), error)) return false;
        manifest.insert("filesLayoutVersion", 1);
    }
    manifest.insert("replicaReady", true);
    const auto bytes = QJsonDocument(manifest).toJson(); QSaveFile output(input.fileName());
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
        setError(error, output.errorString()); return false;
    }
    setError(error, {}); return true;
}

QString SocietyDrive::identifier() const { return m_identifier; }
QString SocietyDrive::displayName() const { return QStringLiteral("Society"); }
QString SocietyDrive::rootPath() const { return m_rootPath; }

bool SocietyDrive::isValid() const
{
    const auto current = open(m_rootPath);
    return current && current->rootPath() == m_rootPath && current->identifier() == m_identifier;
}

bool SocietyDrive::isReady() const
{
    if (!isValid()) return false;
    QFile file(QDir(m_rootPath).filePath(QLatin1String(manifestName)));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return false;
    const auto manifest = QJsonDocument::fromJson(file.readAll()).object();
    return manifest.value("identifier") == m_identifier && manifest.value("replicaReady") != QJsonValue(false);
}

QList<StoreSection> SocietyDrive::sections() const
{
    return isValid() ? allStoreSections() : QList<StoreSection>{};
}

QString SocietyDrive::sectionPath(StoreSection section) const
{
    const auto name = storeSectionName(section);
    return isValid() && !name.isEmpty() ? sectionRoot(m_rootPath, section) : QString();
}

QString SocietyDrive::relativePath(const QString& absolute) const
{
    if (!QDir::isAbsolutePath(absolute) || QDir::cleanPath(absolute) != absolute) return {};
    for (const auto section : allStoreSections()) {
        const auto root = sectionRoot(m_rootPath, section);
        if (root.isEmpty()) continue;
        if (absolute == root) return storeSectionName(section);
        if (absolute.startsWith(root + '/')) return storeSectionName(section) + absolute.mid(root.size());
    }
    return {};
}

QString SocietyDrive::resolvePath(const QString& relative) const
{
    if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.contains('\\') || relative.contains(QChar::Null)) return {};
    for (const auto& part : relative.split('/')) if (part.isEmpty() || part == "." || part == "..") return {};
    for (const auto section : allStoreSections()) {
        const auto name = storeSectionName(section);
        if (relative == name || relative.startsWith(name + '/')) {
            const auto root = sectionRoot(m_rootPath, section);
            return root.isEmpty() ? QString() : root + relative.mid(name.size());
        }
    }
    return {};
}

std::optional<StoreSection> SocietyDrive::sectionForPath(const QString& path) const
{
    if (!isValid()) {
        return std::nullopt;
    }
    const QString relative = relativePath(QFileInfo(QDir(m_rootPath), path).canonicalFilePath());
    const QString firstComponent = relative.section('/', 0, 0);
    for (const auto section : allStoreSections()) {
        if (firstComponent == storeSectionName(section)) {
            return section;
        }
    }
    return std::nullopt;
}

FileTree::EntryResult SocietyDrive::tree(const QString& relative, FileTree::Options options) const
{
    const auto invalid = [] { return FileTree::EntryResult{std::nullopt, std::make_error_code(std::errc::invalid_argument)}; };
    const auto unavailable = [] { return FileTree::EntryResult{std::nullopt, std::make_error_code(std::errc::state_not_recoverable)}; };
    if (!isReady()) return unavailable();
    if (options.maxEntries == 0 || options.maxDepth > 256) return invalid();
    const bool root = relative.isEmpty() || relative == ".";
    if (!root && (resolvePath(relative).isEmpty() || relative.contains(':'))) return invalid();
    auto rebase = [](auto&& self, FileTree::Entry& node, const std::filesystem::path& prefix) -> void {
        node.relativePath = node.relativePath == "." ? prefix : prefix / node.relativePath;
        node.parentPath = node.relativePath.parent_path();
        if (node.parentPath.empty()) node.parentPath = ".";
        for (auto& child : node.children) self(self, child, prefix);
    };
    auto count = [](auto&& self, const FileTree::Entry& node) -> std::size_t {
        std::size_t total = 1;
        for (const auto& child : node.children) total += self(self, child);
        return total;
    };
    FileTree::EntryResult result;
    if (root) {
        result = FileTree(detail::nativePath(m_rootPath)).entry();
        if (!result) return result;
        if (options.maxDepth != 0) {
            std::size_t remaining = options.maxEntries - 1;
            for (const auto section : allStoreSections()) {
                if (remaining == 0) return {std::nullopt, std::make_error_code(std::errc::value_too_large)};
                auto childOptions = options;
                --childOptions.maxDepth;
                childOptions.maxEntries = remaining;
                auto child = FileTree(detail::nativePath(sectionPath(section))).snapshot({}, childOptions);
                if (!child) return child;
                remaining -= count(count, *child.value);
                rebase(rebase, *child.value, detail::nativePath(storeSectionName(section)));
                result.value->children.push_back(std::move(*child.value));
            }
            result.value->childrenLoaded = true;
        }
    } else {
        const auto name = relative.section('/', 0, 0);
        // Resolve the section separately: the Files section may live on another volume.
        const auto nativeRoot = resolvePath(name);
        const auto tail = relative == name ? QString() : relative.mid(name.size() + 1);
        result = FileTree(detail::nativePath(nativeRoot)).snapshot(detail::nativePath(tail), options);
        if (!result) return result;
        rebase(rebase, *result.value, detail::nativePath(name));
    }
    return isReady() ? result : unavailable();
}

FileTree::EntryResult SocietyDrive::entry(const QString& relative) const
{
    return tree(relative, FileTree::Options{false, 0, 1});
}

FileTree::ChildrenResult SocietyDrive::entries(const QString& relative, bool includeHidden) const
{
    auto result = tree(relative, FileTree::Options{includeHidden, 1, 100000});
    if (!result) return {std::nullopt, result.error};
    if (result.value->kind != FileTree::Kind::Directory)
        return {std::nullopt, std::make_error_code(std::errc::not_a_directory)};
    return {std::move(result.value->children), {}};
}

} // namespace iiSocietyContainer
