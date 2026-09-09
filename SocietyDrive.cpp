#include "SocietyDrive.h"

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

QJsonArray sectionDefinitions()
{
    QJsonArray definitions;
    for (const auto section : allStoreSections()) {
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
        return open(root.path(), error);
    }

    for (const auto section : allStoreSections()) {
        const QString path = root.filePath(storeSectionName(section));
        const QFileInfo info(path);
        if ((info.exists() || info.isSymLink() || info.isJunction()) && !isDirectDirectory(path)) {
            setError(error, QStringLiteral("The drive section conflicts with an existing entry: %1").arg(path));
            return std::nullopt;
        }
    }

    QStringList createdDirectories;
    const auto rollback = [&createdDirectories] {
        for (const auto& path : createdDirectories) {
            QDir().rmdir(path); // Only removes empty directories created by this call.
        }
    };
    for (const auto section : allStoreSections()) {
        const QString path = root.filePath(storeSectionName(section));
        if (!QFileInfo::exists(path)) {
            if (!QDir().mkdir(path)) {
                rollback();
                setError(error, QStringLiteral("The drive section could not be created: %1").arg(path));
                return std::nullopt;
            }
            createdDirectories.append(path);
        }
    }

    const QString identifier = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonDocument document(QJsonObject{
        {"type", "SocietyDrive"},
        {"schemaVersion", 1},
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
    const QJsonDocument document = QJsonDocument::fromJson(input.readAll(), &parseError);
    const QJsonObject data = document.object();
    const QString identifier = data.value("identifier").toString();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || data.value("type") != QJsonValue("SocietyDrive")
        || data.value("schemaVersion") != QJsonValue(1)
        || QUuid(identifier).isNull()
        || (data.value("displayName") != QJsonValue("Society")
            && data.value("displayName") != QJsonValue("Society Container"))
        || data.value("sections") != QJsonValue(sectionDefinitions())) {
        setError(error, QStringLiteral("The Society drive manifest is invalid or unsupported."));
        return std::nullopt;
    }
    for (const auto section : allStoreSections()) {
        const QString path = root.filePath(storeSectionName(section));
        if (!isDirectDirectory(path)) {
            setError(error, QStringLiteral("The drive section is missing or redirected: %1").arg(path));
            return std::nullopt;
        }
    }
    return SocietyDrive(root.path(), identifier);
}

QString SocietyDrive::identifier() const { return m_identifier; }
QString SocietyDrive::displayName() const { return QStringLiteral("Society"); }
QString SocietyDrive::rootPath() const { return m_rootPath; }

bool SocietyDrive::isValid() const
{
    const auto current = open(m_rootPath);
    return current && current->rootPath() == m_rootPath && current->identifier() == m_identifier;
}

QList<StoreSection> SocietyDrive::sections() const
{
    return isValid() ? allStoreSections() : QList<StoreSection>{};
}

QString SocietyDrive::sectionPath(StoreSection section) const
{
    const auto name = storeSectionName(section);
    return isValid() && !name.isEmpty() ? QDir(m_rootPath).filePath(name) : QString();
}

std::optional<StoreSection> SocietyDrive::sectionForPath(const QString& path) const
{
    const SocietyContainer container(m_rootPath);
    if (!isValid() || container.classifyPath(path) != SocietyContainer::PathKind::Entry) {
        return std::nullopt;
    }
    const QDir root(m_rootPath);
    const QString relative = root.relativeFilePath(QFileInfo(root, path).canonicalFilePath());
    const QString firstComponent = relative.section('/', 0, 0);
    for (const auto section : allStoreSections()) {
        if (firstComponent == storeSectionName(section)) {
            return section;
        }
    }
    return std::nullopt;
}

} // namespace iiSocietyContainer
