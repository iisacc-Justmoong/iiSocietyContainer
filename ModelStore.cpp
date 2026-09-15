#include "ModelStore.h"
#include "ModelLayout.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <algorithm>

namespace iiSocietyContainer {
namespace {
// This is container data, so it must not use iiSocietySync's local-only .society- prefix.
constexpr auto journalName = ".model-paths.json";
void fail(QString *error, const QString &message) { if (error) *error = message; }
bool safeRelative(const QString &path)
{
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains('\\') || path.contains(':') || path.contains(QChar::Null)) return false;
    for (const auto &part : path.split('/'))
        if (part.isEmpty() || part.startsWith('.')) return false;
    return true;
}
QString confined(const QString &root, const QString &relative, bool createParents = false)
{
    if (!safeRelative(relative)) return {};
    QString current = root;
    const auto parts = relative.split('/');
    for (qsizetype index = 0; index < parts.size(); ++index) {
        current = QDir(current).filePath(parts[index]);
        QFileInfo info(current);
        if (info.isSymLink() || info.isJunction()) return {};
        if (!info.exists() && createParents && index < parts.size() - 1) {
            if (!QDir().mkdir(current)) return {};
            info.refresh();
        }
        if (!info.exists()) return index == parts.size() - 1 ? current : QString();
        if (info.canonicalFilePath() != current || (index < parts.size() - 1 && !info.isDir())) return {};
    }
    return current;
}
QJsonObject journal(const QString &root, const QString &identifier, QString *error)
{
    const auto path = QDir(root).filePath(QLatin1String(journalName));
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink() && !info.isJunction())
        return {{"schemaVersion", 1}, {"containerId", identifier}, {"aliases", QJsonObject()}};
    QFile input(path);
    if (!info.isFile() || info.isSymLink() || info.isJunction() || info.size() > 4 * 1024 * 1024 || !input.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("The model organization journal is unreadable or redirected.")); return {};
    }
    const auto value = QJsonDocument::fromJson(input.readAll()).object();
    if (value.value("schemaVersion") != 1 || value.value("containerId") != identifier || !value.value("aliases").isObject()) {
        fail(error, QStringLiteral("The model organization journal is invalid or belongs to another container.")); return {};
    }
    const auto aliases = value.value("aliases").toObject();
    for (auto it = aliases.begin(); it != aliases.end(); ++it) {
        if (!safeRelative(it.key()) || !safeRelative(it.value().toString())) {
            fail(error, QStringLiteral("The model organization journal contains an invalid path.")); return {};
        }
    }
    return value;
}
bool saveJournal(const QString &root, const QJsonObject &value, QString *error)
{
    QSaveFile file(QDir(root).filePath(QLatin1String(journalName)));
    const auto data = QJsonDocument(value).toJson(QJsonDocument::Compact);
    if (data.size() > 4 * 1024 * 1024 || !file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        fail(error, QStringLiteral("Could not preserve model path references: %1").arg(file.errorString())); return false;
    }
    return true;
}
std::optional<ModelType> category(const QString &relative)
{
    const auto parts = relative.split('/');
    return parts.size() > 1 ? modelTypeFromName(parts.first()) : std::nullopt;
}
}

ModelStore::ModelStore(SocietyDrive drive) : m_drive(std::move(drive)) {}
std::optional<ModelStore> ModelStore::open(const QString &containerPath, QString *error)
{
    if (error) error->clear();
    const auto drive = SocietyDrive::open(containerPath, error);
    if (!drive) return {};
    if (!drive->isReady()) { fail(error, QStringLiteral("The Society mirror is not ready.")); return {}; }
    return ModelStore(*drive);
}
QString ModelStore::rootPath() const { return m_drive.sectionPath(StoreSection::Models); }
QString ModelStore::categoryPath(ModelType type, QString *error) const
{
    if (error) error->clear();
    if (!m_drive.isReady()) { fail(error, QStringLiteral("The Society drive changed or is unavailable.")); return {}; }
    const auto path = confined(rootPath(), modelTypeName(type));
    if (path.isEmpty() || (QFileInfo::exists(path) && !QFileInfo(path).isDir())) {
        fail(error, QStringLiteral("The model category is unavailable or redirected.")); return {};
    }
    return path;
}
bool ModelStore::ensureLayout(QString *error) const
{
    if (error) error->clear();
    if (!m_drive.isReady()) { fail(error, QStringLiteral("The Society drive changed or is unavailable.")); return false; }
    QLockFile lock(QDir(rootPath()).filePath(".society-models.lock"));
    if (!lock.tryLock(5000)) { fail(error, QStringLiteral("Another process is organizing Models.")); return false; }
    return m_drive.isReady() && detail::createModelLayout(rootPath(), error);
}
QList<ModelEntry> ModelStore::scanDirectory(const QString &modelsDirectory, QString *error, const std::atomic_bool *cancelled)
{
    if (error) error->clear();
    const QFileInfo root(modelsDirectory);
    if (modelsDirectory.isEmpty() || !root.isAbsolute() || !root.isDir() || !root.isReadable() || root.isSymLink() || root.isJunction()) {
        fail(error, QStringLiteral("The Models directory is unavailable.")); return {};
    }
    const auto base = root.canonicalFilePath();
    QList<ModelEntry> result;
    QStringList pending{base};
    while (!pending.isEmpty()) {
        if (cancelled && cancelled->load()) return {};
        const QDir directory(pending.takeLast());
        const auto children = directory.entryInfoList(QDir::Files | QDir::Dirs | QDir::Readable | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
        QSet<QString> companions;
        for (const auto &child : children)
            for (const auto &path : ModelClassifier::companionFiles(child.absoluteFilePath())) companions.insert(path);
        for (const auto &child : children) {
            if (cancelled && cancelled->load()) return {};
            const auto path = child.canonicalFilePath();
            if (child.isSymLink() || child.isJunction() || !path.startsWith(base + '/') || companions.contains(path)) continue;
            const bool categoryRoot = directory.path() == base && modelTypeFromName(child.fileName()).has_value();
            const bool package = child.isDir() && !categoryRoot && ModelClassifier::isPackage(path);
            if (child.isDir() && !package) { pending.append(path); continue; }
            if (!package && !child.isFile()) continue;
            const auto relative = QDir(base).relativeFilePath(path);
            const auto assigned = category(relative);
            const auto classification = assigned && *assigned != ModelType::Other
                ? ModelClassification{*assigned, true, "assigned category directory"} : ModelClassifier::classify(path);
            const auto kind = !package ? QStringLiteral("file")
                : QFileInfo::exists(QDir(path).filePath("model_index.json")) ? QStringLiteral("diffusers")
                : QFileInfo::exists(QDir(path).filePath("adapter_config.json")) ? QStringLiteral("adapter") : QStringLiteral("package");
            result.append({path, relative, child.fileName(), kind, child.isFile() ? child.suffix().toLower() : kind, classification});
        }
    }
    if (QFileInfo(modelsDirectory).canonicalFilePath() != base || QFileInfo(modelsDirectory).isSymLink()) {
        fail(error, QStringLiteral("The Models directory changed during enumeration.")); return {};
    }
    std::sort(result.begin(), result.end(), [](const ModelEntry &a, const ModelEntry &b) {
        const auto compared = a.relativePath.compare(b.relativePath, Qt::CaseInsensitive);
        return compared == 0 ? a.relativePath < b.relativePath : compared < 0;
    });
    return result;
}
QList<ModelEntry> ModelStore::entries(QString *error, const std::atomic_bool *cancelled) const
{
    if (!m_drive.isReady()) { fail(error, QStringLiteral("The Society drive changed or is unavailable.")); return {}; }
    const auto result = scanDirectory(rootPath(), error, cancelled);
    if (!m_drive.isReady()) { fail(error, QStringLiteral("The Society drive changed during enumeration.")); return {}; }
    return result;
}
QString ModelStore::resolve(const QString &relativePath, QString *error) const
{
    if (error) error->clear();
    if (!m_drive.isReady() || !safeRelative(relativePath)) { fail(error, QStringLiteral("Invalid model reference.")); return {}; }
    auto relative = relativePath;
    QSet<QString> visited;
    QString localError;
    const auto aliases = journal(rootPath(), m_drive.identifier(), &localError).value("aliases").toObject();
    if (!localError.isEmpty()) { fail(error, localError); return {}; }
    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto path = confined(rootPath(), relative);
        if (!path.isEmpty() && QFileInfo::exists(path)) return path;
        if (visited.contains(relative) || !aliases.contains(relative)) break;
        visited.insert(relative); relative = aliases.value(relative).toString();
    }
    fail(error, QStringLiteral("The model is missing or redirected.")); return {};
}
QString ModelStore::place(const QString &path, std::optional<ModelType> type, QString *error) const
{
    if (error) error->clear();
    if (!m_drive.isReady()) { fail(error, QStringLiteral("The Society drive changed or is unavailable.")); return {}; }
    const auto base = rootPath();
    QLockFile lock(QDir(base).filePath(".society-models.lock"));
    if (!lock.tryLock(5000)) { fail(error, QStringLiteral("Another process is organizing Models.")); return {}; }
    const auto relative = QDir::isAbsolutePath(path) ? QDir(base).relativeFilePath(path) : path;
    const auto source = confined(base, relative);
    if (source.isEmpty() || !QFileInfo::exists(source)
        || (QFileInfo(source).isDir() && !relative.contains('/') && modelTypeFromName(relative).has_value())
        || (QFileInfo(source).isDir() && !ModelClassifier::isPackage(source))) {
        fail(error, QStringLiteral("Choose an existing model file or package inside Models.")); return {};
    }
    const auto assigned = category(relative);
    const auto resolvedType = type.value_or(assigned && *assigned != ModelType::Other ? *assigned : ModelClassifier::classify(source).type);
    auto tail = relative;
    if (assigned) tail = relative.section('/', 1);
    const auto targetRelative = modelTypeName(resolvedType) + '/' + tail;
    if (relative == targetRelative) return source;
    if (!m_drive.isReady() || !detail::createModelLayout(base, error)) return {};
    QString localError;
    const auto previousJournal = journal(base, m_drive.identifier(), &localError);
    if (!localError.isEmpty()) { fail(error, localError); return {}; }
    auto aliases = previousJournal.value("aliases").toObject();
    const auto companions = ModelClassifier::companionFiles(source);
    const QFileInfo destinationInfo(targetRelative);
    const auto parent = destinationInfo.path();
    for (int index = 0; index < 10000; ++index) {
        const auto name = index == 0 ? destinationInfo.fileName()
            : QFileInfo(source).isDir() ? QString("%1 (%2)").arg(destinationInfo.fileName()).arg(index)
            : destinationInfo.completeBaseName() + QString(" (%1)").arg(index)
                + (destinationInfo.suffix().isEmpty() ? QString() : '.' + destinationInfo.suffix());
        const auto candidateRelative = QDir(parent).filePath(name);
        const auto destination = confined(base, candidateRelative, true);
        if (destination.isEmpty()) { fail(error, QStringLiteral("The target category or parent directory is redirected.")); return {}; }
        QList<QPair<QString, QString>> moves{{source, destination}};
        for (const auto &companion : companions) {
            const auto fullPrefix = companion.startsWith(source + '.');
            const auto oldPrefix = fullPrefix ? source : QDir(QFileInfo(source).absolutePath()).filePath(QFileInfo(source).completeBaseName());
            const auto newPrefix = fullPrefix ? destination : QDir(QFileInfo(destination).absolutePath()).filePath(QFileInfo(destination).completeBaseName());
            moves.append({companion, newPrefix + companion.mid(oldPrefix.size())});
        }
        if (std::any_of(moves.cbegin(), moves.cend(), [](const auto &move) {
                const QFileInfo target(move.second); return target.exists() || target.isSymLink() || target.isJunction();
            })) continue;
        for (const auto &move : moves) aliases.insert(QDir(base).relativeFilePath(move.first), QDir(base).relativeFilePath(move.second));
        auto updated = previousJournal; updated.insert("aliases", aliases);
        // Journal first: an interruption resolves the old path until its rename completes.
        if (!m_drive.isReady() || !saveJournal(base, updated, error)) return {};
        QList<QPair<QString, QString>> moved;
        for (const auto &move : moves) {
            if (!m_drive.isReady() || confined(base, QDir(base).relativeFilePath(move.first)) != move.first
                || confined(base, QDir(base).relativeFilePath(move.second)) != move.second || !QDir().rename(move.first, move.second)) {
                bool rolledBack = true;
                for (auto it = moved.crbegin(); it != moved.crend(); ++it) rolledBack = QDir().rename(it->second, it->first) && rolledBack;
                if (rolledBack) saveJournal(base, previousJournal, nullptr);
                fail(error, QStringLiteral("Could not move the complete model and its companions. Existing files were not replaced.")); return {};
            }
            moved.append(move);
        }
        return destination;
    }
    fail(error, QStringLiteral("No unused model filename is available.")); return {};
}
ModelOrganization ModelStore::organize(const std::atomic_bool *cancelled) const
{
    ModelOrganization report;
    QString error;
    if (!ensureLayout(&error)) { report.errors.append(error); return report; }
    const auto models = entries(&error, cancelled);
    if (!error.isEmpty()) { report.errors.append(error); return report; }
    for (const auto &model : models) {
        if (cancelled && cancelled->load()) { report.cancelled = true; break; }
        const auto assigned = category(model.relativePath);
        if (assigned && *assigned != ModelType::Other) continue;
        const auto placed = place(model.path, model.classification.type, &error);
        if (placed.isEmpty()) report.errors.append(error);
        else if (placed != model.path) report.moved.append({model.path, placed, model.classification.type});
    }
    if (cancelled && cancelled->load()) report.cancelled = true;
    return report;
}
}
