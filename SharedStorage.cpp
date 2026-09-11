#include "SharedStorage.h"

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
bool inventory(const QString &path, const QString &base, QCryptographicHash &hash)
{
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isReadable() || info.canonicalFilePath() != path)
        return false;
    if (info.isDir()) {
        for (const auto &entry : QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name))
            if (!inventory(entry.absoluteFilePath(), base, hash))
                return false;
        return true;
    }
    if (!info.isFile())
        return false;
    hash.addData(QDir(base).relativeFilePath(path).toUtf8());
    hash.addData(QByteArray::number(info.size()));
    hash.addData(QByteArray::number(info.lastModified().toMSecsSinceEpoch()));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    hash.addData(file.read(65536));
    return file.error() == QFile::NoError;
}
std::optional<StoredModel> inspect(const QString &path, const QString &base)
{
    const QFileInfo info(path);
    const bool package = info.isDir() && QFileInfo::exists(QDir(path).filePath("model_index.json"));
    const auto extension = info.suffix().toLower();
    if (!package && (!info.isFile() || (extension != "safetensor" && extension != "safetensors")))
        return {};
    QCryptographicHash fingerprint(QCryptographicHash::Sha256);
    if (!inventory(path, base, fingerprint))
        return {};
    return StoredModel{QDir(base).relativeFilePath(path), info.fileName(),
        package ? QStringLiteral("diffusers") : QStringLiteral("safetensors"),
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
    const auto bytes = QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"containerId", drive->identifier()},
        {"path", drive->rootPath()}}).toJson();
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
        if (settings.value("schemaVersion").toInt() != 1 || expectedId.isEmpty() || selected.isEmpty()) {
            fail(error, QStringLiteral("The shared Society settings are invalid."));
            return {};
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
    QString current = base;
    for (const auto &part : id.split('/')) {
        current = QDir(current).filePath(part);
        const QFileInfo info(current);
        if (info.isSymLink() || info.canonicalFilePath() != current) {
            fail(error, QStringLiteral("The Society model is missing or redirected."));
            return {};
        }
    }
    const auto model = inspect(current, base);
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
