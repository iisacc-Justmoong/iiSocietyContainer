#include "SharedStorage.h"
#include "ModelStore.h"
#include "StorageMap.h"
#include "DiskImage.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

namespace iiSocietyContainer {
#ifdef Q_OS_IOS
QString iosSharedStorageRoot(QString *error);
#endif
#ifdef Q_OS_ANDROID
QString androidSharedStorageRoot(QString *error);
#endif
namespace {
void fail(QString *error, const QString &message) { if (error) *error = message; }
bool relative(const QString &path, bool allowHidden = false)
{
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains('\\')
        || path.contains(':') || path.contains(QChar::Null))
        return false;
    for (const auto &part : path.split('/'))
        if (part.isEmpty() || part == "." || part == ".." || (!allowHidden && part.startsWith('.')))
            return false;
    return true;
}
bool inventory(const QString &path, const QString &base, QCryptographicHash &hash,
    const QString &physicalRoot = {}, const QString &logicalRoot = {})
{
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isReadable() || info.canonicalFilePath() != path)
        return false;
    if (info.isDir()) {
        for (const auto &entry : QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name))
            if (!inventory(entry.absoluteFilePath(), base, hash, physicalRoot, logicalRoot))
                return false;
        return true;
    }
    if (!info.isFile())
        return false;
    const auto relative = logicalRoot.isEmpty() ? QDir(base).relativeFilePath(path)
        : logicalRoot + (path == physicalRoot ? QString() : '/' + QDir(physicalRoot).relativeFilePath(path));
    hash.addData(relative.toUtf8());
    hash.addData(QByteArray::number(info.size()));
    hash.addData(QByteArray::number(info.lastModified().toMSecsSinceEpoch()));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    hash.addData(file.read(65536));
    return file.error() == QFile::NoError;
}
std::optional<StoredModel> inspect(const QString &path, const QString &base, const QString &previousId = {})
{
    const QFileInfo info(path);
    const bool package = info.isDir() && QFileInfo::exists(QDir(path).filePath("model_index.json"));
    const auto extension = info.suffix().toLower();
    if (!package && (!info.isFile() || (extension != "safetensor" && extension != "safetensors")))
        return {};
    QCryptographicHash fingerprint(QCryptographicHash::Sha256);
    if (!inventory(path, base, fingerprint, path, previousId))
        return {};
    QString format = package ? QStringLiteral("diffusers") : QStringLiteral("safetensors");
    if (package) {
        QFile manifest(QDir(path).filePath("model_index.json"));
        if (manifest.size() <= 1024 * 1024 && manifest.open(QIODevice::ReadOnly)) {
            const auto object = QJsonDocument::fromJson(manifest.readAll()).object();
            if (object.value("schema") == "iild-unified-model-v1" && object.value("_class_name") == "IILDUnifiedCascade")
                format = QStringLiteral("unified");
        }
    }
    return StoredModel{previousId.isEmpty() ? QDir(base).relativeFilePath(path) : previousId, info.fileName(),
        format,
        QString::fromLatin1(fingerprint.result().toHex())};
}
}

QJsonObject StoredModel::reference(const QString &containerId) const
{
    return {{"containerId", containerId}, {"path", id}, {"format", format}, {"fingerprint", fingerprint}};
}
QString SharedStorage::settingsPath()
{
    const auto override = qEnvironmentVariable("SOCIETY_STORAGE_SETTINGS_PATH");
    if (!override.isEmpty())
        return QDir::isAbsolutePath(override) ? QDir::cleanPath(override) : QString();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath("iisacc/Society/storage.json");
}
bool SharedStorage::setDefaultContainer(const QString &path, QString *error)
{
    if (error) error->clear();
    const auto drive = SocietyDrive::open(path, error);
    if (!drive)
        return false;
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
#ifdef Q_OS_IOS
    const auto shared = iosSharedStorageRoot(error);
#else
    const auto shared = androidSharedStorageRoot(error);
#endif
    if (shared.isEmpty() || drive->rootPath() != shared) {
        fail(error, QStringLiteral("Mobile clients must use the managed Society container."));
        return false;
    }
    return true;
#else
    const auto settings = settingsPath();
    if (settings.isEmpty() || !QDir().mkpath(QFileInfo(settings).absolutePath())) {
        fail(error, QStringLiteral("The shared Society settings directory is unavailable."));
        return false;
    }
    QLockFile lock(settings + ".lock");
    if (!lock.tryLock(1000)) {
        fail(error, QStringLiteral("Another app is updating the shared Society container."));
        return false;
    }
    QSaveFile file(settings);
    QJsonObject configuration{{"schemaVersion", 1}, {"containerId", drive->identifier()}, {"path", drive->rootPath()}};
    if (const auto volume = DiskImage::mountedAt(drive->rootPath().toStdString())) {
        configuration.insert("schemaVersion", 2);
        configuration.insert("imagePath", QString::fromStdString(volume->imagePath.string()));
    }
    const auto bytes = QJsonDocument(configuration).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        fail(error, file.errorString());
        return false;
    }
    return true;
#endif
}
std::optional<SharedStorage> SharedStorage::open(const QString &path, QString *error)
{
    return open(path, error, false);
}
std::optional<SharedStorage> SharedStorage::open(const QString &path, QString *error, bool allowIncompleteReplica)
{
    if (error) error->clear();
    QString selected = path;
    QString expectedId;
    if (selected.isEmpty())
        selected = qEnvironmentVariable("SOCIETY_CONTAINER_PATH");
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
#ifdef Q_OS_IOS
    const auto shared = iosSharedStorageRoot(error);
#else
    const auto shared = androidSharedStorageRoot(error);
#endif
    if (shared.isEmpty())
        return {};
    if (selected.isEmpty())
        selected = shared;
    if (QFileInfo(selected).canonicalFilePath() != shared) {
        fail(error, QStringLiteral("Use the managed Society container on this mobile platform."));
        return {};
    }
#endif
    if (selected.isEmpty()) {
        QFile file(settingsPath());
        if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) {
            fail(error, QStringLiteral("Open a container in Society, or choose its source folder."));
            return {};
        }
        const auto settings = QJsonDocument::fromJson(file.readAll()).object();
        selected = settings.value("path").toString();
        expectedId = settings.value("containerId").toString();
        const int version = settings.value("schemaVersion").toInt();
        if ((version != 1 && version != 2) || expectedId.isEmpty() || selected.isEmpty()) {
            fail(error, QStringLiteral("The shared Society settings are invalid."));
            return {};
        }
        if (version == 2) {
            const auto image = settings.value("imagePath").toString();
            if (!QDir::isAbsolutePath(image)) {
                fail(error, QStringLiteral("The saved Society disk image path is invalid.")); return {};
            }
            const auto volume = DiskImage::mount(image.toStdString());
            if (!volume) { fail(error, QString::fromStdString(volume.error())); return {}; }
            selected = QString::fromStdString(volume->mountPath.string());
        }
    }
    const auto drive = SocietyDrive::open(selected, error);
    if (!drive)
        return {};
    if (!expectedId.isEmpty() && drive->identifier() != expectedId) {
        QFile manifest(QDir(drive->rootPath()).filePath(".society-drive.json"));
        QJsonObject identity;
        if (manifest.open(QIODevice::ReadOnly) && manifest.size() <= 65536) identity = QJsonDocument::fromJson(manifest.readAll()).object();
        if (identity.value("localIdentifier") != expectedId && identity.value("previousIdentifier") != expectedId) {
            fail(error, QStringLiteral("The configured Society container identity changed. Reopen it in Society."));
            return {};
        }
    }
    if (!allowIncompleteReplica && !drive->isReady()) {
        fail(error, QStringLiteral("The host drive's initial mirror is not ready."));
        return {};
    }
    return SharedStorage(*drive);
}
SharedStorage::SharedStorage(SocietyDrive drive) : m_drive(std::move(drive)) {}
const SocietyDrive &SharedStorage::drive() const { return m_drive; }
QList<StoredModel> SharedStorage::models(QString *error) const
{
    if (!m_drive.isReady()) { fail(error, QStringLiteral("The Society mirror is not ready.")); return {}; }
    if (error) error->clear();
    QList<StoredModel> result;
    StorageMap map(m_drive);
    const auto objects = map.objects(error);
    if (!objects.isEmpty()) {
        QMap<QString, QJsonObject> entries;
        QStringList packages;
        for (const auto &value : objects) {
            const auto e = value.toObject(); const auto key = e.value("path").toString();
            if (!key.startsWith("models/") || e.value("kind") != "file") continue;
            bool hidden = false; for (const auto &part : key.mid(7).split('/')) hidden |= part.startsWith('.');
            if (hidden) continue;
            entries.insert(key, e);
            if (key.endsWith("/model_index.json")) packages.append(key.chopped(17));
        }
        const auto append = [&](const QString &key, bool package) {
            QCryptographicHash fingerprint(QCryptographicHash::Sha256); bool available = true; qint64 size = 0;
            for (auto it = entries.lowerBound(package ? key + '/' : key); it != entries.cend(); ++it) {
                if (it.key() != key && !(package && it.key().startsWith(key + '/'))) break;
                fingerprint.addData(it.key().toUtf8()); fingerprint.addData(it.value().value("version").toString().toUtf8());
                available &= map.isResident(it.value()); size += it.value().value("size").toString().toLongLong();
            }
            QString format = package ? "diffusers" : "safetensors";
            if (package) {
                QFile manifest(map.localPath(key + "/model_index.json"));
                if (manifest.size() <= 1024 * 1024 && manifest.open(QIODevice::ReadOnly)
                    && QJsonDocument::fromJson(manifest.readAll()).object().value("schema") == "iild-unified-model-v1") format = "unified";
            }
            result.append({key.mid(7), key.section('/', -1), format,
                QString::fromLatin1(fingerprint.result().toHex()), available, size});
        };
        packages.sort();
        for (const auto &key : packages) {
            bool nested = false; for (const auto &other : packages) if (key.startsWith(other + '/')) nested = true;
            if (!nested) append(key, true);
        }
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            if (!it.key().endsWith(".safetensors", Qt::CaseInsensitive) && !it.key().endsWith(".safetensor", Qt::CaseInsensitive)) continue;
            bool nested = false; for (const auto &package : packages) if (it.key().startsWith(package + '/')) nested = true;
            if (!nested) append(it.key(), false);
        }
        std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
        return result;
    }
    const auto base = m_drive.sectionPath(StoreSection::Models);
    if (base.isEmpty()) {
        fail(error, QStringLiteral("The Society Models area is unavailable."));
        return result;
    }
    const auto visit = [&](auto &&self, const QString &directory) -> void {
        for (const auto &entry : QDir(directory).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name)) {
            if (entry.isSymLink() || entry.fileName().startsWith('.'))
                continue;
            if (auto model = inspect(entry.absoluteFilePath(), base))
                result.append(*model);
            else if (entry.isDir() && !QFileInfo::exists(QDir(entry.absoluteFilePath()).filePath("model_index.json")))
                self(self, entry.absoluteFilePath());
        }
    };
    visit(visit, base);
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.id.compare(right.id, Qt::CaseInsensitive) < 0;
    });
    return result;
}
QString SharedStorage::resolveModel(const QJsonObject &reference, QString *error) const
{
    if (!m_drive.isReady()) { fail(error, QStringLiteral("The Society mirror is not ready.")); return {}; }
    if (error) error->clear();
    const auto id = reference.value("path").toString();
    const auto base = m_drive.sectionPath(StoreSection::Models);
    if (base.isEmpty() || reference.value("containerId").toString() != m_drive.identifier() || !relative(id)) {
        fail(error, QStringLiteral("The model reference does not belong to this Society container."));
        return {};
    }
    StorageMap map(m_drive);
    if (!map.objects().isEmpty()) {
        for (const auto &model : models(error)) {
            if (model.id != id) continue;
            if (model.reference(m_drive.identifier()) != reference) {
                fail(error, "The Society model changed after it was selected. Select it again."); return {};
            }
            if (!model.available) { fail(error, "Download this model from its Society host before generation."); return {}; }
            return map.localPath("models/" + id);
        }
        fail(error, "The selected model is no longer in the Society storage map."); return {};
    }
    const auto store = ModelStore::open(m_drive.rootPath(), error);
    if (!store) return {};
    const auto current = store->resolve(id, error);
    if (current.isEmpty()) return {};
    // Classification changes the location, not the bytes or the original reference contract.
    const auto model = inspect(current, base, id);
    if (!model || model->reference(m_drive.identifier()) != reference) {
        fail(error, QStringLiteral("The Society model changed after it was selected. Select it again."));
        return {};
    }
    return current;
}
QString SharedStorage::filePath(StoreSection section, const QString &relativePath, QString *error) const
{
    if (!m_drive.isReady()) { fail(error, QStringLiteral("The Society mirror is not ready.")); return {}; }
    if (error) error->clear();
    auto current = m_drive.sectionPath(section);
    if (current.isEmpty() || (!relativePath.isEmpty() && !relative(relativePath, true))) {
        fail(error, QStringLiteral("Use a relative path inside an available Society area."));
        return {};
    }
    if (relativePath.isEmpty())
        return current;
    const auto parts = relativePath.split('/');
    for (qsizetype index = 0; index < parts.size(); ++index) {
        current = QDir(current).filePath(parts[index]);
        const QFileInfo info(current);
        const bool last = index == parts.size() - 1;
        if (info.isSymLink() || info.isJunction()) {
            fail(error, QStringLiteral("The Society path is redirected: %1").arg(parts[index]));
            return {};
        }
        if (last && !info.exists())
            return current;
        if (info.canonicalFilePath() != current || (!last && !info.isDir())
            || (last && !info.isDir() && !info.isFile())) {
            fail(error, QStringLiteral("The Society path is unavailable: %1").arg(parts[index]));
            return {};
        }
    }
    return current;
}
QString SharedStorage::ensureDirectory(StoreSection section, const QString &relativePath, QString *error) const
{
    if (!m_drive.isReady()) { fail(error, QStringLiteral("The Society mirror is not ready.")); return {}; }
    if (error) error->clear();
    auto current = m_drive.sectionPath(section);
    if (current.isEmpty() || !relative(relativePath, true)) {
        fail(error, QStringLiteral("Use a relative directory inside a valid Society area."));
        return {};
    }
    for (const auto &part : relativePath.split('/')) {
        current = QDir(current).filePath(part);
        QFileInfo info(current);
        if (!info.exists() && !info.isSymLink() && !info.isJunction()) {
            QDir().mkdir(current);
            info.refresh();
        }
        if (!info.isDir() || info.isSymLink() || info.isJunction() || info.canonicalFilePath() != current) {
            fail(error, QStringLiteral("The Society directory is unavailable or redirected: %1").arg(part));
            return {};
        }
    }
    return current;
}
}
