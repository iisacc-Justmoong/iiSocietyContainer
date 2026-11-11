#include <iiFileProviderDevice.h>
#include "ModelStore.h"
#include "ModelLayout.h"
#include "FileTree.h"
#include "SharedStorage.h"
#include "src/AssetCompanions.h"
#include "src/ExclusiveRename.h"
#include "src/FileTreePaths.h"
#include <FileCatalog.h>
#include <iiFileProviderPresentation.h>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <exception>
#include <filesystem>

namespace iiSocietyContainer {
namespace {
// This is container data, so it must not use iiSocietyHelper::Sync's local-only .society- prefix.
constexpr auto journalName = ".model-paths.json";
void fail(QString *error, const QString &message) { if (error) *error = message; }
bool providerControl(const QString &name)
{
    return iiFileProvider::native::Presentation::isControlFile(std::filesystem::path(name.toStdU16String()));
}
bool safeRelative(const QString &path)
{
    if (path.isEmpty() || iiFileProvider::Directory::isAbsolutePath(path) || path.contains('\\') || path.contains(':') || path.contains(QChar::Null)) return false;
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
        current = iiFileProvider::Directory(current).filePath(parts[index]);
        QFileInfo info(current);
        if (info.isSymLink() || info.isJunction()) return {};
        if (!info.exists() && createParents && index < parts.size() - 1) {
            if (!iiFileProvider::Directory().mkdir(current)) return {};
            info.refresh();
        }
        if (!info.exists()) return index == parts.size() - 1 ? current : QString();
        if (info.canonicalFilePath() != current || (index < parts.size() - 1 && !info.isDir())) return {};
    }
    return current;
}
bool confinedMovePath(const QString &root, const QString &path)
{
    const auto relative = iiFileProvider::Directory(root).relativeFilePath(path);
    if (!detail::isPortablePreviewPath(path)) return confined(root, relative) == path;
    const QFileInfo info(path);
    const auto parent = info.absolutePath();
    const auto parentRelative = iiFileProvider::Directory(root).relativeFilePath(parent);
    if (parent != root && confined(root, parentRelative) != parent) return false;
    return !info.isSymLink() && !info.isJunction() && (!info.exists() || info.canonicalFilePath() == path);
}
bool confinedPrivateMovePath(const QString &root, const QString &path) {
    if (!path.startsWith(root + '/') || QDir::cleanPath(path) != path) return false;
    QString current = root;
    const QFileInfo base(root);
    if (!base.isDir() || base.isSymLink() || base.isJunction() || base.canonicalFilePath() != root) return false;
    const auto parts = path.mid(root.size() + 1).split('/');
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (parts[i].isEmpty() || parts[i] == "." || parts[i] == "..") return false;
        current += '/' + parts[i];
        const QFileInfo info(current);
        if (info.isSymLink() || info.isJunction() || (info.exists() && info.canonicalFilePath() != current)) return false;
        if (i + 1 < parts.size() && !info.isDir()) return false;
    }
    return true;
}
QJsonObject journal(const QString &root, const QString &identifier, QString *error)
{
    const auto path = iiFileProvider::Directory(root).filePath(QLatin1String(journalName));
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink() && !info.isJunction())
        return {{"schemaVersion", 1}, {"containerId", identifier}, {"aliases", QJsonObject()}};
    iiFileProvider::FileDevice input(path);
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
    if (value.contains("manualCategories")) {
        if (!value.value("manualCategories").isObject()) {
            fail(error, QStringLiteral("The model organization journal contains invalid manual categories.")); return {};
        }
        const auto manual = value.value("manualCategories").toObject();
        for (auto it = manual.begin(); it != manual.end(); ++it) {
            if (!safeRelative(it.key()) || !it.value().isString() || !modelTypeFromName(it.value().toString())) {
                fail(error, QStringLiteral("The model organization journal contains invalid manual categories.")); return {};
            }
        }
    }
    return value;
}
bool saveJournal(const QString &root, const QJsonObject &value, QString *error)
{
    iiFileProvider::SaveFile file(iiFileProvider::Directory(root).filePath(QLatin1String(journalName)));
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
    const auto base = rootPath();
    iiFileProvider::LockFile lock(iiFileProvider::Directory(base).filePath(".society-models.lock"));
    if (!lock.tryLock(5000)) { fail(error, QStringLiteral("Another process is organizing Models.")); return {}; }
    if (!m_drive.isReady() || !detail::ensureModelCategory(base, type, error)) return {};
    const auto path = confined(base, modelTypeName(type));
    if (path.isEmpty() || (QFileInfo::exists(path) && !QFileInfo(path).isDir())) {
        fail(error, QStringLiteral("The model category is unavailable or redirected.")); return {};
    }
    return path;
}
bool ModelStore::ensureLayout(QString *error) const
{
    return ensureLayout(error, {});
}
bool ModelStore::ensureLayout(QString *error, std::stop_token stop) const
{
    if (error) error->clear();
    if (!m_drive.isReady()) { fail(error, QStringLiteral("The Society drive changed or is unavailable.")); return false; }
    iiFileProvider::LockFile lock(iiFileProvider::Directory(rootPath()).filePath(".society-models.lock"));
    QElapsedTimer timer; timer.start();
    while (!stop.stop_requested() && !lock.tryLock(50)) {
        if (timer.elapsed() >= 5000) { fail(error, QStringLiteral("Another process is organizing Models.")); return false; }
    }
    if (stop.stop_requested()) { fail(error, QStringLiteral("Model organization cancelled.")); return false; }
    return m_drive.isReady() && detail::validateModelLayout(rootPath(), error);
}
QList<ModelEntry> ModelStore::scanDirectory(const QString &modelsDirectory, QString *error, const std::atomic_bool *cancelled)
{
    return scanDirectory(modelsDirectory, error, cancelled, {});
}
QList<ModelEntry> ModelStore::scanDirectory(const QString &modelsDirectory, QString *error,
                                            const std::atomic_bool *cancelled, std::stop_token stop)
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
        if (stop.stop_requested() || (cancelled && cancelled->load())) return {};
        const iiFileProvider::Directory directory(pending.takeLast());
        const auto children = directory.entryInfoList(iiFileProvider::Directory::Files | iiFileProvider::Directory::Dirs | iiFileProvider::Directory::Readable | iiFileProvider::Directory::NoDotAndDotDot | iiFileProvider::Directory::NoSymLinks, iiFileProvider::Directory::Name);
        QSet<QString> companions;
        for (const auto &child : children)
            for (const auto &path : ModelClassifier::companionFiles(child.absoluteFilePath())) companions.insert(path);
        for (const auto &child : children) {
            if (stop.stop_requested() || (cancelled && cancelled->load())) return {};
            // Dot-prefixed metadata is not marked Hidden on Windows.
            if (child.fileName().startsWith('.') || providerControl(child.fileName())) continue;
            const auto path = child.canonicalFilePath();
            if (child.isSymLink() || child.isJunction() || !path.startsWith(base + '/') || companions.contains(path)) continue;
            const bool categoryRoot = directory.path() == base && modelTypeFromName(child.fileName()).has_value();
            const bool package = child.isDir() && !categoryRoot && ModelClassifier::isPackage(path);
            if (child.isDir() && !package) { pending.append(path); continue; }
            if (!package && !child.isFile()) continue;
            const auto relative = iiFileProvider::Directory(base).relativeFilePath(path);
            const auto assigned = category(relative);
            const auto classification = assigned && *assigned != ModelType::Other
                ? ModelClassification{*assigned, true, "assigned category directory"} : ModelClassifier::classify(path);
            const auto kind = !package ? QStringLiteral("file")
                : QFileInfo::exists(iiFileProvider::Directory(path).filePath("model_index.json")) ? QStringLiteral("diffusers")
                : QFileInfo::exists(iiFileProvider::Directory(path).filePath("adapter_config.json")) ? QStringLiteral("adapter") : QStringLiteral("package");
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
    return entries(error, cancelled, {});
}
QList<ModelEntry> ModelStore::entries(QString *error, const std::atomic_bool *cancelled, std::stop_token stop) const
{
    if (!m_drive.isReady()) { fail(error, QStringLiteral("The Society drive changed or is unavailable.")); return {}; }
    const auto result = scanDirectory(rootPath(), error, cancelled, stop);
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
QString ModelStore::indexAsset(const QString &path, QString *error) const
{
    if (error) error->clear();
    if (!m_drive.isReady()) { fail(error, QStringLiteral("The Society drive changed or is unavailable.")); return {}; }
    const auto base = rootPath();
    const auto relative = iiFileProvider::Directory::isAbsolutePath(path)
        ? iiFileProvider::Directory(base).relativeFilePath(path) : path;
    const auto asset = confined(base, relative);
    const QFileInfo info(asset);
    if (asset.isEmpty() || !info.isFile() || info.isSymLink() || info.isJunction()
        || providerControl(info.fileName())) {
        fail(error, QStringLiteral("Choose a resident model asset inside Models.")); return {};
    }
    try {
        const auto nativeRoot = detail::nativePath(base);
        const auto entry = FileTree(nativeRoot).entry(std::filesystem::path(relative.toStdU16String()));
        if (!entry || entry.value->kind != FileTree::Kind::File)
            throw std::runtime_error("The native model spelling could not be verified.");
        const auto spelling = entry.value->path.lexically_relative(nativeRoot).generic_u8string();
        std::string key(spelling.begin(), spelling.end());
        iiFileProvider::FileCatalog catalog(nativeRoot, iiFileProvider::FileCatalog::Access::Create,
            FileRepository::classificationDirectory(detail::nativePath(m_drive.rootPath()), "models"));
        auto previous = catalog.lookupPath(key);
#ifdef Q_OS_MACOS
        if (!previous) {
            const auto composed = QString::fromUtf8(key).normalized(QString::NormalizationForm_C).toUtf8().toStdString();
            if (composed != key) previous = catalog.lookupPath(composed);
        }
#endif
        if (previous) {
            try { (void)catalog.read(previous->key, 0, 1); }
            catch (const std::exception &) {
                fail(error, QStringLiteral("A different file already has this catalog path. Resolve the existing entry before indexing."));
                return {};
            }
            key = previous->path;
        }
        const auto indexed = catalog.registerFile(key);
        if (!indexed.metadataWritten) {
            fail(error, QString::fromStdString(indexed.metadataError)); return {};
        }
        return QString::fromStdString(indexed.file.key);
    } catch (const std::exception &failure) {
        fail(error, QString::fromUtf8(failure.what())); return {};
    }
}

QString ModelStore::place(const QString &path, std::optional<ModelType> type, QString *error) const
{
    return relocate(path, type, {}, error);
}
QString ModelStore::rename(const QString &path, const QString &name, QString *error,
                           QList<QPair<QString, QString>> *movedAssets) const
{
    if (error) error->clear();
    if (movedAssets) movedAssets->clear();
    if (name.isEmpty() || name.startsWith('.') || name.contains('/') || name.contains('\\')
        || name.contains(':') || name.contains(QChar::Null) || providerControl(name)) {
        fail(error, QStringLiteral("Enter a valid model file name.")); return {};
    }
    return relocate(path, {}, name, error, movedAssets);
}
QStringList ModelStore::repairCompanions(QStringList *errors, const std::atomic_bool *cancelled) const
{
    return repairCompanions(errors, cancelled, {});
}
QStringList ModelStore::repairCompanions(QStringList *errors, const std::atomic_bool *cancelled, std::stop_token stop) const
{
    QString error;
    const auto storage = SharedStorage::open(m_drive.rootPath(), &error);
    if (!storage) { if (errors) *errors = {error}; return {}; }
    return storage->repairModelCompanions(errors, cancelled, stop);
}
QString ModelStore::relocate(const QString &path, std::optional<ModelType> type,
                             const std::optional<QString> &name, QString *error,
                             QList<QPair<QString, QString>> *movedAssets) const
{
    if (error) error->clear();
    if (!m_drive.isReady()) { fail(error, QStringLiteral("The Society drive changed or is unavailable.")); return {}; }
    const auto base = rootPath();
    iiFileProvider::LockFile lock(iiFileProvider::Directory(base).filePath(".society-models.lock"));
    if (!lock.tryLock(5000)) { fail(error, QStringLiteral("Another process is organizing Models.")); return {}; }
    const auto relative = iiFileProvider::Directory::isAbsolutePath(path) ? iiFileProvider::Directory(base).relativeFilePath(path) : path;
    for (const auto &part : relative.split('/')) {
        if (providerControl(part)) {
            fail(error, QStringLiteral("Provider metadata cannot be organized as a model.")); return {};
        }
    }
    const auto source = confined(base, relative);
    if (source.isEmpty() || !QFileInfo::exists(source)
        || (QFileInfo(source).isDir() && !relative.contains('/') && modelTypeFromName(relative).has_value())
        || (QFileInfo(source).isDir() && !ModelClassifier::isPackage(source))) {
        fail(error, QStringLiteral("Choose an existing model file or package inside Models.")); return {};
    }
    const auto assigned = category(relative);
    const auto resolvedType = name ? assigned.value_or(ModelType::Other)
        : type.value_or(assigned && *assigned != ModelType::Other ? *assigned : ModelClassifier::classify(source).type);
    auto tail = relative;
    if (assigned) tail = relative.section('/', 1);
    const auto parentRelative = QFileInfo(relative).path();
    const auto targetRelative = name ? (parentRelative == "." ? *name : parentRelative + '/' + *name)
                                     : modelTypeName(resolvedType) + '/' + tail;
    if (!m_drive.isReady() || !detail::validateModelLayout(base, error)) return {};
    QString localError;
    const auto previousJournal = journal(base, m_drive.identifier(), &localError);
    if (!localError.isEmpty()) { fail(error, localError); return {}; }
    std::unique_ptr<iiFileProvider::FileCatalog> catalog;
    try {
        const auto root = std::filesystem::path(base.toStdU16String());
        if (std::filesystem::exists(root / ".iisacc-file-provider/catalog.sqlite3"))
            catalog = std::make_unique<iiFileProvider::FileCatalog>(root, iiFileProvider::FileCatalog::Access::ReadOnly);
    } catch (const std::exception &failure) {
        fail(error, QString::fromUtf8(failure.what())); return {};
    }
    // Recheck under the model lock: a user may have chosen Other after an
    // organizer took its earlier directory snapshot.
    if (!name && !type && assigned && *assigned == ModelType::Other
        && previousJournal.value("manualCategories").toObject().value(relative).toString() == modelTypeName(*assigned))
        return source;
    if (relative == targetRelative) {
        // An explicit selection of Other is meaningful even when the model is
        // already there: automatic organization must respect that choice.
        if (!name && type && *type == ModelType::Other) {
            auto manual = previousJournal.value("manualCategories").toObject();
            if (manual.value(relative).toString() != modelTypeName(*type)) {
                manual.insert(relative, modelTypeName(*type));
                auto updated = previousJournal; updated.insert("manualCategories", manual);
                if (!saveJournal(base, updated, error)) return {};
            }
        }
        return source;
    }
    auto aliases = previousJournal.value("aliases").toObject();
    const QFileInfo destinationInfo(targetRelative);
    const auto parent = destinationInfo.path();
    for (int index = 0; index < 10000; ++index) {
        if (name && index > 0) { fail(error, QStringLiteral("An item with that name already exists.")); return {}; }
        const auto candidateName = index == 0 ? destinationInfo.fileName()
            : QFileInfo(source).isDir() ? QString("%1 (%2)").arg(destinationInfo.fileName()).arg(index)
            : destinationInfo.completeBaseName() + QString(" (%1)").arg(index)
                + (destinationInfo.suffix().isEmpty() ? QString() : '.' + destinationInfo.suffix());
        const auto candidateRelative = parent == "." ? candidateName : iiFileProvider::Directory(parent).filePath(candidateName);
        const auto destination = confined(base, candidateRelative, true);
        if (destination.isEmpty()) { fail(error, QStringLiteral("The target category or parent directory is redirected.")); return {}; }
        const auto object = detail::AssetObject::plan(source, destination, true, true, error);
        if (!object) return {};
        const auto &moves = object->members;
        const auto &assets = object->assets;
        if (std::any_of(object->reservedTargets.cbegin(), object->reservedTargets.cend(), [](const auto &path) {
                const QFileInfo target(path); return target.exists() || target.isSymLink() || target.isJunction();
            })) continue;
        if (catalog) {
            bool reserved = false;
            try {
                const auto lookup = [&](const QString &relative) {
                    auto result = catalog->lookupPath(relative.toUtf8().toStdString());
#ifdef Q_OS_MACOS
                    if (!result) {
                        const auto decomposed = relative.normalized(QString::NormalizationForm_D);
                        if (decomposed != relative) result = catalog->lookupPath(decomposed.toUtf8().toStdString());
                    }
#endif
                    return result;
                };
                for (const auto &asset : assets) {
                    const auto from = iiFileProvider::Directory(base).relativeFilePath(asset.first);
                    const auto to = iiFileProvider::Directory(base).relativeFilePath(asset.second);
                    const auto prior = lookup(from);
                    const auto target = lookup(to);
                    if (target && (!prior || target->key != prior->key)) { reserved = true; break; }
                }
            } catch (const std::exception &failure) {
                fail(error, QString::fromUtf8(failure.what())); return {};
            }
            if (reserved) continue;
        }
        for (const auto &move : assets) aliases.insert(iiFileProvider::Directory(base).relativeFilePath(move.first), iiFileProvider::Directory(base).relativeFilePath(move.second));
        auto updated = previousJournal; updated.insert("aliases", aliases);
        auto manual = previousJournal.value("manualCategories").toObject();
        const auto previousCategory = manual.value(relative);
        manual.remove(relative);
        if (name && previousCategory.isString()) manual.insert(candidateRelative, previousCategory);
        else if (type && *type == ModelType::Other) manual.insert(candidateRelative, modelTypeName(*type));
        if (!manual.isEmpty() || previousJournal.contains("manualCategories")) updated.insert("manualCategories", manual);
        // Journal first: an interruption resolves the old path until its rename completes.
        if (!m_drive.isReady() || !saveJournal(base, updated, error)) return {};
        QList<QPair<QString, QString>> moved;
        const auto rename = [&](const QString &from, const QString &to) {
            return detail::renameWithoutCopy(from, to);
        };
        const auto privateRoot = QString::fromStdU16String(FileRepository::classificationDirectory(
            detail::nativePath(m_drive.rootPath()), "models").u16string());
        for (const auto &move : moves) {
            const bool publicPair = confinedMovePath(base, move.first) && confinedMovePath(base, move.second);
            const bool privatePair = confinedPrivateMovePath(privateRoot, move.first)
                                  && confinedPrivateMovePath(privateRoot, move.second);
            const bool legacyMigration = move.first.endsWith(".iisacc.json", Qt::CaseInsensitive)
                && confinedMovePath(base, move.first) && confinedPrivateMovePath(privateRoot, move.second);
            if (!m_drive.isReady() || (!publicPair && !privatePair && !legacyMigration) || !rename(move.first, move.second)) {
                bool rolledBack = true;
                for (auto it = moved.crbegin(); it != moved.crend(); ++it) rolledBack = rename(it->second, it->first) && rolledBack;
                if (rolledBack) saveJournal(base, previousJournal, nullptr);
                fail(error, rolledBack
                    ? QStringLiteral("Could not move the complete model and its companions. Existing files were not replaced.")
                    : QStringLiteral("The model move could not be fully rolled back. Check both %1 and %2 before retrying.").arg(source, destination));
                return {};
            }
            moved.append(move);
        }
        if (movedAssets) *movedAssets = assets;
        return destination;
    }
    fail(error, QStringLiteral("No unused model filename is available.")); return {};
}
ModelOrganization ModelStore::organize(const std::atomic_bool *cancelled) const
{
    return organize(cancelled, {});
}
ModelOrganization ModelStore::organize(const std::atomic_bool *cancelled, std::stop_token stop) const
{
    ModelOrganization report;
    QString error;
    if (!ensureLayout(&error, stop)) {
        if (stop.stop_requested() || (cancelled && cancelled->load())) report.cancelled = true;
        else report.errors.append(error);
        return report;
    }
    (void)repairCompanions(&report.errors, cancelled, stop);
    if (stop.stop_requested() || (cancelled && cancelled->load())) { report.cancelled = true; return report; }
    const auto models = entries(&error, cancelled, stop);
    if (stop.stop_requested() || (cancelled && cancelled->load())) { report.cancelled = true; return report; }
    if (!error.isEmpty()) { report.errors.append(error); return report; }
    const auto data = journal(rootPath(), m_drive.identifier(), &error);
    if (!error.isEmpty()) { report.errors.append(error); return report; }
    const auto manual = data.value("manualCategories").toObject();
    for (const auto &model : models) {
        if (stop.stop_requested() || (cancelled && cancelled->load())) { report.cancelled = true; break; }
        const auto assigned = category(model.relativePath);
        if (assigned && *assigned != ModelType::Other) continue;
        if (assigned && manual.value(model.relativePath).toString() == modelTypeName(*assigned)) continue;
        const auto placed = place(model.path, {}, &error);
        if (placed.isEmpty()) report.errors.append(error);
        else if (placed != model.path) report.moved.append({model.path, placed, model.classification.type});
    }
    if (stop.stop_requested() || (cancelled && cancelled->load())) report.cancelled = true;
    return report;
}
}
