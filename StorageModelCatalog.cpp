#include "StorageModelCatalog.h"
#include <ModelStore.h>
#include <SharedStorage.h>
#include <StorageMap.h>

#include <QDirIterator>
#include <QFileInfo>
#include <QThread>
#include <QJsonArray>
#include <QLocale>
#include <QSet>

#include <algorithm>

namespace iiSocietyContainer {
namespace {
QVariantMap emptyGroups()
{
    QVariantMap groups;
    for (const auto type : allModelTypes()) groups.insert(modelTypeName(type), QVariantList());
    return groups;
}
QString firstString(const QJsonObject &metadata, const QStringList &keys)
{
    for (const auto &key : keys) {
        const auto value = metadata.value(key).toString().trimmed();
        if (!value.isEmpty()) return value.left(256);
    }
    return {};
}
QString architecture(const QJsonObject &metadata)
{
    auto value = firstString(metadata, {"modelspec.architecture", "architecture", "general.architecture", "_class_name"});
    if (value.isEmpty()) value = metadata.value("architectures").toArray().first().toString().left(256);
    if (value.isEmpty()) value = metadata.value("model_type").toString().left(256);
    if (value.contains("stable-diffusion-xl", Qt::CaseInsensitive) || value.contains("StableDiffusionXL")) return "SDXL";
    return value;
}
QString categoryForPath(const QString &relative)
{
    const auto type = relative.contains('/') ? modelTypeFromName(relative.section('/', 0, 0)) : std::nullopt;
    return modelTypeName(type.value_or(ModelType::Other));
}
// Catalog filtering is presentation-only: leave documents in physical storage.
// Replicas apply the same filename contract without downloading tensor payloads.
bool catalogFile(const QString &relative)
{
    const auto type = modelTypeFromName(categoryForPath(relative)).value_or(ModelType::Other);
    const auto suffix = QFileInfo(relative).suffix().toLower();
    if (type == ModelType::Other) return true;
    if (QStringList{"safetensors", "safetensor", "gguf", "ggml", "bin", "ckpt", "pt", "pth",
        "onnx", "pb", "tflite", "h5", "hdf5", "npz", "npy", "model", "mlmodel", "engine"}.contains(suffix)) return true;
    if (type == ModelType::Wildcards) return suffix == "txt" || suffix == "wildcards";
    if (type == ModelType::Poses || type == ModelType::Workflows || type == ModelType::ComfyUIWorkflows)
        return QStringList{"json", "png", "jpg", "jpeg", "webp"}.contains(suffix);
    return false;
}
QString precision(const QJsonObject &metadata)
{
    auto value = firstString(metadata, {"modelspec.precision", "precision", "torch_dtype", "dtype"});
    if (value.isEmpty()) {
        QStringList types;
        for (const auto &dtype : metadata.value("tensor_dtypes").toArray()) types.append(dtype.toString());
        value = types.join(" / ");
    }
    value.replace("bfloat16", "BF16", Qt::CaseInsensitive);
    value.replace("float16", "FP16", Qt::CaseInsensitive);
    value.replace("float32", "FP32", Qt::CaseInsensitive);
    if (value == "F16") value = "FP16";
    if (value == "F32") value = "FP32";
    if (value.isEmpty() && metadata.contains("general.file_type")) {
        const auto type = metadata.value("general.file_type").toInt(-1);
        value = type == 0 ? "FP32" : type == 1 ? "FP16" : QObject::tr("Quantized");
    }
    return value;
}
struct Snapshot {
    QVariantMap groups = emptyGroups();
    int count = 0, uncategorized = 0;
    QString error;
    QStringList watches;
};
void sortRows(Snapshot &result) {
    for (auto it = result.groups.begin(); it != result.groups.end(); ++it) {
        auto rows = it.value().toList();
        std::sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
            const auto left = a.toMap(), right = b.toMap();
            const auto compared = left.value("name").toString().compare(right.value("name").toString(), Qt::CaseInsensitive);
            return compared == 0 ? left.value("path").toString() < right.value("path").toString() : compared < 0;
        });
        it.value() = rows;
    }
}
Snapshot fromMap(const SocietyDrive &drive, const QJsonArray &objects) {
    Snapshot result;
    StorageMap map(drive); const bool authority = map.isLocalAuthority();
    result.watches = {QDir(drive.rootPath()).filePath("Models"), QDir(drive.rootPath()).filePath(".society-sync")};
    QMap<QString, QJsonObject> files;
    QStringList packages;
    for (const auto &value : objects) {
        const auto e = value.toObject(); const auto key = e.value("path").toString();
        if (!key.startsWith("models/") || e.value("kind") != "file" || StorageMap::physicalPath(key).isEmpty()) continue;
        // Metadata remains sufficient for replica browsing. The host additionally
        // checks existence so a filesystem move takes effect before reindexing.
        if (authority && !QFileInfo::exists(map.localPath(key))) continue;
        bool hidden = false;
        for (const auto &part : key.mid(7).split('/')) hidden |= part.startsWith('.');
        if (hidden) continue;
        files.insert(key, e);
        if (key.endsWith("/model_index.json") || key.endsWith("/adapter_config.json")) packages.append(key.left(key.lastIndexOf('/')));
    }
    if (authority) {
        // A new multi-GB import can precede the sync index's full-file hash by
        // minutes. Merge shallow directory metadata so local publication is
        // immediately visible, without opening tensor headers or packages.
        const auto modelsRoot = QDir(drive.rootPath()).filePath("Models");
        for (const auto type : allModelTypes()) {
            const auto category = modelTypeName(type);
            const QDir directory(QDir(modelsRoot).filePath(category));
            const QFileInfo folder(directory.path());
            if (!folder.isDir() || folder.isSymLink() || folder.isJunction()) continue;
            result.watches.append(directory.path());
            for (const auto &file : directory.entryInfoList(QDir::Files | QDir::NoSymLinks)) {
                const auto relative = category + '/' + file.fileName();
                if (file.isJunction() || !catalogFile(relative)) continue;
                const auto key = "models/" + relative;
                files.insert(key, QJsonObject{{"path", key}, {"kind", "file"},
                    {"size", QString::number(file.size())}, {"resident", true}});
            }
        }
    }
    QSet<QString> companions;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const auto &key = it.key();
        const auto stem = key.left(key.lastIndexOf('/') + 1) + QFileInfo(key).completeBaseName();
        for (const auto &prefix : {key, stem})
            for (const auto *suffix : {".model.json", ".civitai.info", ".preview.png", ".preview.jpg", ".preview.webp"}) {
                const auto companion = prefix + QLatin1String(suffix);
                if (companion != key && files.contains(companion)) companions.insert(companion);
            }
        if (key.endsWith("/config.json")) {
            const auto parent = key.left(key.lastIndexOf('/'));
            for (auto child = files.lowerBound(parent + '/'); child != files.cend() && child.key().startsWith(parent + '/'); ++child) {
                const auto name = child.key().mid(parent.size() + 1);
                if (!name.contains('/') && QStringList{"safetensors", "safetensor", "bin", "gguf"}.contains(QFileInfo(name).suffix())) {
                    packages.append(parent); break;
                }
            }
        }
    }
    // Type directories are containers, never packages themselves.
    packages.removeIf([](const QString &path) {
        const auto relative = path.mid(7);
        return !relative.contains('/') && modelTypeFromName(relative).has_value();
    });
    packages.removeDuplicates(); packages.sort();
    QStringList roots;
    for (const auto &package : packages) {
        bool nested = false;
        for (const auto &root : roots) if (package.startsWith(root + '/')) { nested = true; break; }
        if (!nested) roots.append(package);
    }
    const auto append = [&](const QString &key, bool package) {
        const auto relative = key.mid(7), name = relative.section('/', -1);
        const auto path = QDir(drive.rootPath()).filePath(StorageMap::physicalPath(key));
        const auto group = categoryForPath(relative);
        const auto format = package ? (name.endsWith(".iildmodel") ? QString("Unified") : QString("Diffusers"))
            : (name.endsWith(".safetensors", Qt::CaseInsensitive) || name.endsWith(".safetensor", Qt::CaseInsensitive)) ? QString("Safetensors") : QFileInfo(name).suffix().toUpper();
        qint64 bytes = 0; bool resident = true;
        for (auto it = files.lowerBound(package ? key + '/' : key); it != files.cend(); ++it) {
            if (it.key() != key && !(package && it.key().startsWith(key + '/'))) break;
            bytes += it.value().value("size").toString().toLongLong();
            resident &= it.value().value("resident").toBool();
        }
        auto rows = result.groups.value(group).toList();
        rows.append(QVariantMap{{"path", path}, {"folderPath", QFileInfo(path).absolutePath()}, {"name", name},
            {"relativePath", relative}, {"type", group}, {"directory", package}, {"architecture", QObject::tr("Unknown")},
            {"precision", QObject::tr("Unknown")}, {"format", format}, {"bytes", bytes}, {"available", resident},
            {"sizeText", resident ? QObject::tr("%1 on device").arg(QLocale().formattedDataSize(bytes))
                : QObject::tr("%1 · download when used").arg(QLocale().formattedDataSize(bytes))}});
        result.groups.insert(group, rows); ++result.count;
        if (group == modelTypeName(ModelType::Other)) ++result.uncategorized;
    };
    for (const auto &root : roots) append(root, true);
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        if (companions.contains(it.key())) continue;
        bool nested = false;
        for (const auto &root : roots) if (it.key().startsWith(root + '/')) { nested = true; break; }
        if (!nested && catalogFile(it.key().mid(7))) append(it.key(), false);
    }
    sortRows(result);
    return result;
}
Snapshot scan(const QString &directory, const std::shared_ptr<std::atomic_bool> &cancel)
{
    Snapshot result;
    // A synchronized catalog is the browsing source of truth. Never open a
    // tensor header or walk a model package merely to display its identity.
    if (const auto drive = SocietyDrive::open(QFileInfo(directory).absolutePath())) {
        if (QFileInfo::exists(QDir(drive->rootPath()).filePath(".society-sync/catalog.json"))) {
            const auto objects = StorageMap(*drive).objects(&result.error);
            if (!result.error.isEmpty() || cancel->load()) return result;
            return fromMap(*drive, objects);
        }
    }
    const auto entries = ModelStore::scanDirectory(directory, &result.error, cancel.get());
    if (!result.error.isEmpty() || cancel->load()) return result;
    const auto root = QFileInfo(directory).canonicalFilePath();
    result.watches.append(root);
    QDirIterator directories(root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (directories.hasNext()) {
        if (cancel->load()) return {};
        directories.next();
        const auto info = directories.fileInfo();
        if (!info.isSymLink() && info.canonicalFilePath().startsWith(root + '/')) result.watches.append(info.canonicalFilePath());
    }
    for (const auto &entry : entries) {
        if (cancel->load()) return {};
        if (entry.kind == "file" && !catalogFile(entry.relativePath)) continue;
        const auto metadata = ModelClassifier::metadata(entry.path);
        const auto group = categoryForPath(entry.relativePath);
        result.watches.append(entry.path);
        result.watches.append(ModelClassifier::companionFiles(entry.path));
        if (entry.kind != "file") {
            for (const auto *name : {"config.json", "model_index.json", "adapter_config.json", "society.model.json"}) {
                const QFileInfo info(QDir(entry.path).filePath(QLatin1String(name)));
                if (info.isFile() && !info.isSymLink()) result.watches.append(info.absoluteFilePath());
            }
        }
        if (group == modelTypeName(ModelType::Other)) ++result.uncategorized;
        qint64 bytes = 0;
        if (entry.kind == "file") bytes = QFileInfo(entry.path).size();
        else {
            QDirIterator files(entry.path, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
            while (files.hasNext()) {
                if (cancel->load()) return {};
                files.next();
                const auto info = files.fileInfo();
                if (!info.isSymLink() && info.canonicalFilePath().startsWith(entry.path + '/')) bytes += info.size();
            }
        }
        auto name = firstString(metadata, {"modelspec.title", "general.name"});
        if (name.isEmpty()) name = entry.kind == "file" ? QFileInfo(entry.name).completeBaseName() : entry.name;
        const auto modelArchitecture = architecture(metadata);
        const auto modelPrecision = precision(metadata);
        auto rows = result.groups.value(group).toList();
        rows.append(QVariantMap{{"path", entry.path}, {"folderPath", QFileInfo(entry.path).absolutePath()},
            {"name", name}, {"relativePath", entry.relativePath}, {"type", group}, {"directory", entry.kind != "file"},
            {"architecture", modelArchitecture.isEmpty() ? QObject::tr("Unknown") : modelArchitecture},
            {"precision", modelPrecision.isEmpty() ? QObject::tr("Unknown") : modelPrecision},
            {"format", entry.format == "safetensors" || entry.format == "safetensor" ? QString("Safetensors")
                : entry.kind == "diffusers" ? QString("Diffusers") : entry.kind == "adapter" ? QString("PEFT")
                : entry.kind == "package" ? QString("Transformers") : entry.format.toUpper()},
            {"bytes", bytes}, {"sizeText", QObject::tr("%1 on device").arg(QLocale().formattedDataSize(bytes))}});
        result.groups.insert(group, rows);
        ++result.count;
    }
    sortRows(result);
    result.watches.removeDuplicates();
    if (result.watches.size() > 64) result.watches = result.watches.mid(0, 64);
    return result;
}
}

QVariantList StorageModelCatalog::categories() const
{
    QVariantList result;
    for (const auto type : allModelTypes()) {
        const auto name = modelTypeName(type);
        result.append(QVariantMap{{"key", name}, {"title", name}});
    }
    return result;
}

StorageModelCatalog::StorageModelCatalog(QObject *parent) : QObject(parent), m_groups(emptyGroups())
{
    m_debounce.setSingleShot(true); m_debounce.setInterval(150);
    connect(&m_files, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_files, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_debounce, &QTimer::timeout, this, &StorageModelCatalog::refresh);
    m_poll.setInterval(30000); connect(&m_poll, &QTimer::timeout, this, &StorageModelCatalog::refresh);
    m_opener = new StorageDirectoryModel(this);
    connect(m_opener, &StorageDirectoryModel::activated, this, [this](const QString &path, bool directory) {
        if (path != m_requestedPath) return;
        m_downloadStatus = tr("Available on this device"); emit downloadStatusChanged(); refresh();
        if (m_openWhenReady) emit objectReady(path, directory);
    });
    connect(m_opener, &StorageDirectoryModel::downloadFailed, this, [this](const QString &error) {
        if (m_requestedPath.isEmpty()) return;
        m_downloadStatus = error; emit downloadStatusChanged();
    });
}
StorageModelCatalog::~StorageModelCatalog() { if (m_cancel) m_cancel->store(true); }
void StorageModelCatalog::setDirectory(const QString &directory)
{
    if (m_directory == directory) return;
    if (m_cancel) m_cancel->store(true);
    ++m_revision; m_refreshPending = false; m_scanning = false; m_hasSnapshot = false;
    m_debounce.stop(); m_poll.stop();
    m_openWhenReady = false; m_downloadStatus.clear(); m_requestedPath.clear(); emit downloadStatusChanged();
    if (m_loading) { m_loading = false; emit loadingChanged(); }
    const auto watches = m_files.files() + m_files.directories();
    if (!watches.isEmpty()) m_files.removePaths(watches);
    emit modelsAboutToChange();
    m_directory = directory; m_groups = emptyGroups(); m_error.clear(); m_count = m_uncategorized = 0;
    emit directoryChanged(); emit modelsChanged();
    if (!directory.isEmpty()) m_poll.start();
    refresh();
}
void StorageModelCatalog::activatePath(const QString &path, bool openWhenReady)
{
    for (const auto &group : m_groups) for (const auto &value : group.toList()) {
        const auto row = value.toMap();
        if (row.value("path").toString() != path) continue;
        m_openWhenReady = openWhenReady;
        m_requestedPath = path;
        m_downloadStatus = tr("Downloading from the Society host…"); emit downloadStatusChanged();
        m_opener->openPath(path, row.value("directory").toBool()); return;
    }
}
void StorageModelCatalog::refresh()
{
    if (m_scanning) { m_refreshPending = true; return; }
    if (m_directory.isEmpty()) return;
    const auto revision = ++m_revision;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    m_scanning = true;
    if (!m_hasSnapshot) { m_loading = true; emit loadingChanged(); }
    const auto snapshotResult = std::make_shared<Snapshot>();
    const auto directory = m_directory; const auto cancel = m_cancel;
    auto *worker = QThread::create([snapshotResult, directory, cancel] { *snapshotResult = scan(directory, cancel); });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, snapshotResult, revision] {
        const auto &snapshot = *snapshotResult;
        if (revision != m_revision) return;
        const auto watches = m_files.files() + m_files.directories();
        const QSet<QString> previous(watches.cbegin(), watches.cend());
        const QSet<QString> next(snapshot.watches.cbegin(), snapshot.watches.cend());
        const auto removed = (previous - next).values(), added = (next - previous).values();
        if (!removed.isEmpty()) m_files.removePaths(removed);
        if (!added.isEmpty()) m_files.addPaths(added);
        if (m_groups != snapshot.groups || m_error != snapshot.error || m_uncategorized != snapshot.uncategorized) {
            emit modelsAboutToChange();
            m_groups = snapshot.groups; m_error = snapshot.error;
            m_count = snapshot.count; m_uncategorized = snapshot.uncategorized;
            emit modelsChanged();
        }
        m_scanning = false; m_hasSnapshot = true;
        if (m_loading) { m_loading = false; emit loadingChanged(); }
        if (m_refreshPending) { m_refreshPending = false; m_debounce.start(); }
    });
    worker->start();
}

}
