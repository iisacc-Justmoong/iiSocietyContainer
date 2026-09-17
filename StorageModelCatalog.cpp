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
    return {{"image", QVariantList()}, {"video", QVariantList()}, {"audio", QVariantList()}, {"language", QVariantList()}};
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
QString modality(const ModelEntry &entry, const QJsonObject &metadata)
{
    const auto explicitValue = firstString(metadata, {"society.modality", "modelspec.modality", "modality"}).toLower();
    if (emptyGroups().contains(explicitValue)) return explicitValue;
    const auto hint = (architecture(metadata) + ' ' + metadata.value("pipeline_tag").toString()).toLower();
    const auto has = [&](const QStringList &words) {
        return std::any_of(words.cbegin(), words.cend(), [&](const QString &word) { return hint.contains(word); });
    };
    if (has({"video", "animatediff", "cogvideo", "wanpipeline", "wantransformer", "hunyuanvideo"})) return "video";
    if (has({"audio", "speech", "whisper", "bark", "musicgen", "vits", "tacotron"})) return "audio";
    if (has({"causallm", "text-generation", "llama", "mistral", "gemma", "qwen", "llava"})) return "language";
    if (has({"diffusion", "sdxl", "flux", "text-to-image", "image-to-image"})) return "image";
    switch (entry.classification.type) {
    case ModelType::LLM: case ModelType::VLM: return "language";
    case ModelType::Motion: return "video";
    case ModelType::Other: return {};
    default: return "image";
    }
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
    result.watches = {QDir(drive.rootPath()).filePath("Models"), QDir(drive.rootPath()).filePath(".society-sync")};
    QMap<QString, QJsonObject> files;
    QStringList packages;
    for (const auto &value : objects) {
        const auto e = value.toObject(); const auto key = e.value("path").toString();
        if (!key.startsWith("models/") || e.value("kind") != "file" || StorageMap::physicalPath(key).isEmpty()) continue;
        bool hidden = false;
        for (const auto &part : key.mid(7).split('/')) hidden |= part.startsWith('.');
        if (hidden) continue;
        files.insert(key, e);
        if (key.endsWith("/model_index.json") || key.endsWith("/adapter_config.json")) packages.append(key.left(key.lastIndexOf('/')));
    }
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
        const auto category = relative.section('/', 0, 0).toLower();
        const auto format = package ? (name.endsWith(".iildmodel") ? QString("Unified") : QString("Diffusers"))
            : (name.endsWith(".safetensors", Qt::CaseInsensitive) || name.endsWith(".safetensor", Qt::CaseInsensitive)) ? QString("Safetensors") : QFileInfo(name).suffix().toUpper();
        const auto group = category == "llm" || category == "vlm" || format == "GGUF" ? QString("language")
            : category == "motion" ? QString("video") : QString("image");
        qint64 bytes = 0; bool resident = true;
        for (auto it = files.lowerBound(package ? key + '/' : key); it != files.cend(); ++it) {
            if (it.key() != key && !(package && it.key().startsWith(key + '/'))) break;
            bytes += it.value().value("size").toString().toLongLong();
            resident &= it.value().value("resident").toBool();
        }
        auto rows = result.groups.value(group).toList();
        rows.append(QVariantMap{{"path", path}, {"folderPath", QFileInfo(path).absolutePath()}, {"name", name},
            {"relativePath", relative}, {"directory", package}, {"architecture", QObject::tr("Unknown")},
            {"precision", QObject::tr("Unknown")}, {"format", format}, {"bytes", bytes}, {"available", resident},
            {"sizeText", resident ? QObject::tr("%1 on device").arg(QLocale().formattedDataSize(bytes))
                : QObject::tr("%1 · download when used").arg(QLocale().formattedDataSize(bytes))}});
        result.groups.insert(group, rows); ++result.count;
    };
    for (const auto &root : roots) append(root, true);
    const QStringList extensions{"safetensors", "safetensor", "ckpt", "pt", "pth", "bin", "gguf", "onnx"};
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        if (!extensions.contains(QFileInfo(it.key()).suffix().toLower())) continue;
        bool nested = false;
        for (const auto &root : roots) if (it.key().startsWith(root + '/')) { nested = true; break; }
        if (!nested) append(it.key(), false);
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
    const QStringList extensions{"safetensors", "safetensor", "ckpt", "pt", "pth", "bin", "gguf", "onnx"};
    for (const auto &entry : entries) {
        if (cancel->load()) return {};
        if (entry.kind == "file" && !extensions.contains(entry.format)) continue;
        const auto metadata = ModelClassifier::metadata(entry.path);
        const auto group = modality(entry, metadata);
        result.watches.append(entry.path);
        result.watches.append(ModelClassifier::companionFiles(entry.path));
        if (entry.kind != "file") {
            for (const auto *name : {"config.json", "model_index.json", "adapter_config.json", "society.model.json"}) {
                const QFileInfo info(QDir(entry.path).filePath(QLatin1String(name)));
                if (info.isFile() && !info.isSymLink()) result.watches.append(info.absoluteFilePath());
            }
        }
        if (group.isEmpty()) { ++result.uncategorized; continue; }
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
            {"name", name}, {"relativePath", entry.relativePath}, {"directory", entry.kind != "file"},
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
    ++m_revision; m_refreshPending = false; m_debounce.stop(); m_poll.stop();
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
    if (m_loading) { m_refreshPending = true; return; }
    if (m_directory.isEmpty()) return;
    const auto revision = ++m_revision;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    m_loading = true; emit loadingChanged();
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
        m_loading = false; emit loadingChanged();
        if (m_refreshPending) { m_refreshPending = false; m_debounce.start(); }
    });
    worker->start();
}

}
