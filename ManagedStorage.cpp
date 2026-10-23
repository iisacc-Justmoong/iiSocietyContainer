#include "SharedStorage.h"
#include "src/FileTreePaths.h"
#include <ObjectSource.h>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QSysInfo>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace iiSocietyContainer {
using namespace iiFileProvider;
namespace {
std::string utf8(const QString &value) { return value.toUtf8().toStdString(); }
QString qtext(const std::string &value) { return QString::fromUtf8(value); }
QString number(std::uint64_t value) { return QString::number(value); }
std::uint64_t unsignedValue(const QJsonValue &value, std::uint64_t fallback = 0) {
    if (value.isUndefined()) return fallback;
    bool ok = false; const auto parsed = value.toVariant().toString().toULongLong(&ok);
    if (!ok || parsed > std::uint64_t(INT64_MAX)) throw std::runtime_error("Invalid search counter."); return parsed;
}
std::string stringValue(const QJsonValue &value) {
    if (!value.isString()) throw std::runtime_error("Search fields require strings."); return utf8(value.toString());
}
std::vector<std::string> strings(const QJsonValue &value) {
    if (value.isUndefined()) return {};
    if (!value.isArray()) throw std::runtime_error("Search terms require an array.");
    std::vector<std::string> result; for (const auto &v : value.toArray()) result.push_back(stringValue(v)); return result;
}
QJsonObject row(const ObjectRecord &file) {
    return {{"objectKey", qtext(file.key)}, {"path", qtext(file.path)}, {"version", number(file.version)},
        {"sessionKey", qtext(file.sessionKey)}, {"sha256", qtext(file.sha256)}, {"size", number(file.size)}};
}
QJsonObject cursorJson(const FileCursor &cursor) {
    return {{"catalog", qtext(cursor.catalog)}, {"revision", number(cursor.revision)},
        {"afterIndex", number(cursor.afterIndex)}, {"filter", qtext(cursor.filter)}};
}
QJsonObject cursorJson(const OntologyCursor &cursor) {
    return {{"revision", number(cursor.revision)}, {"fileSequence", number(cursor.fileSequence)},
        {"afterIndexKey", number(cursor.afterIndexKey)}, {"fingerprint", qtext(cursor.fingerprint)}};
}
OntologyValue ontologyValue(const QJsonObject &value) {
    const auto kind = value.value("kind").toString();
    if (kind == "text") return OntologyValue::text(stringValue(value.value("value")));
    if (kind == "integer") {
        bool ok = false; const auto n = value.value("value").toVariant().toString().toLongLong(&ok);
        if (!ok) throw std::runtime_error("Invalid ontology integer."); return OntologyValue::integer(n);
    }
    if (kind == "real" && value.value("value").isDouble()) return OntologyValue::real(value.value("value").toDouble());
    if (kind == "boolean" && value.value("value").isBool()) return OntologyValue::boolean(value.value("value").toBool());
    if (kind == "file" || kind == "entity") return OntologyValue::resource({kind == "file" ? OntologyResourceKind::File : OntologyResourceKind::Entity, stringValue(value.value("key"))});
    throw std::runtime_error("Unknown typed ontology value.");
}
}
std::unique_ptr<FileRepository> SharedStorage::fileRepository(FileRepositoryContext context, FileRepository::Access access) const {
    if (!m_drive.isReady()) throw std::runtime_error("The Society drive is not ready.");
    if (context.work.application.empty()) context.work.application = utf8(QCoreApplication::applicationName().isEmpty() ? QStringLiteral("Society") : QCoreApplication::applicationName());
    if (context.work.applicationVersion.empty()) context.work.applicationVersion = utf8(QCoreApplication::applicationVersion());
    if (context.work.project.empty()) context.work.project = utf8(m_drive.identifier());
    if (context.device.empty()) context.device = utf8(QSysInfo::machineHostName());
    std::vector<FileRepositorySection> sections;
    for (const auto section : m_drive.sections()) sections.push_back({utf8(storeSectionKey(section)), detail::nativePath(m_drive.sectionPath(section))});
    const auto pinned = m_drive;
    return std::make_unique<FileRepository>(detail::nativePath(m_drive.rootPath()), utf8(m_drive.identifier()),
        std::move(sections), std::move(context), access, [pinned] { return pinned.isReady(); });
}
std::string SharedStorage::managedFilePath(StoreSection section, const QString &relative) const {
    QString error; const auto path = filePath(section, relative, &error);
    if (path.isEmpty() || relative.isEmpty()) throw std::runtime_error("Choose an available public file path.");
    const auto root = detail::nativePath(m_drive.sectionPath(section));
    auto native = detail::nativePath(path);
    if (QFileInfo(path).exists()) {
        const auto entry = FileTree(root).entry(detail::nativePath(relative));
        if (!entry) throw std::runtime_error("The native file spelling is unavailable."); native = entry.value->path;
    } else native = std::filesystem::canonical(native.parent_path()) / native.filename();
    const auto text = native.lexically_relative(root).generic_u8string();
    return utf8(storeSectionKey(section)) + '/' + std::string(text.begin(), text.end());
}
bool SharedStorage::recordFile(StoreSection section, const QString &relative, QString *error, FileRepositoryContext context) const {
    return recordFile(section, relative, error, std::move(context), {});
}
bool SharedStorage::recordFile(StoreSection section, const QString &relative, QString *error, FileRepositoryContext context, std::stop_token stop) const {
    try {
        if (stop.stop_requested()) throw std::runtime_error("file recording cancelled");
        const auto path = filePath(section, relative, error); if (path.isEmpty() || relative.isEmpty()) return false;
        if (context.work.document.empty()) context.work.document = utf8(storeSectionKey(section) + '/' + relative);
        fileRepository(std::move(context))->observe(managedFilePath(section, relative), {}, stop);
        if (error) error->clear(); return true;
    } catch (const std::exception &e) { if (error) *error = QString::fromUtf8(e.what()); return false; }
}
bool SharedStorage::reconcileFiles(QString *error, std::stop_token stop) const {
    try {
        FileRepositoryContext context; context.description = "Native file intake";
        fileRepository(std::move(context))->reconcile(stop); if (error) error->clear(); return true;
    } catch (const std::exception &e) { if (error) *error = QString::fromUtf8(e.what()); return false; }
}
QJsonObject SharedStorage::fileTracking(StoreSection section, const QString &relative, QString *error) const {
    try {
        if (filePath(section, relative, error).isEmpty()) return {};
        if (!std::filesystem::exists(FileRepository::packagePath(detail::nativePath(m_drive.rootPath()), utf8(m_drive.identifier())))) return {{"status", "pending"}};
        auto repository = fileRepository({}, FileRepository::Access::ReadOnly);
        const auto current = repository->lookup(managedFilePath(section, relative));
        if (!current) return {{"status", "pending"}};
        const auto original = repository->original(current->key);
        const auto work = repository->objects().workSession(current->sessionKey);
        auto result = row(*current); result.insert("status", "ready"); result.insert("provider", "iiFileProvider");
        if (ObjectSource::inspect(detail::nativePath(filePath(section, relative))).stamp != current->sourceStamp) result.insert("status", "pending");
        result.insert("rawKey", qtext(original.record.key)); result.insert("rawVersion", number(original.record.version));
        result.insert("originalName", qtext(original.metadata.originalName)); result.insert("originalSha256", qtext(original.record.sha256));
        result.insert("application", qtext(work.context.application)); result.insert("document", qtext(work.context.document));
        result.insert("sessionDescription", qtext(work.session.description));
        result.insert("sessionStartedAtNs", QString::number(work.session.startedAtNs));
        result.insert("ontologyRevision", number(repository->ontology().revision()));
        if (error) error->clear(); return result;
    } catch (const std::exception &e) { if (error) *error = QString::fromUtf8(e.what()); return {}; }
}
QJsonObject SharedStorage::searchFiles(const QJsonObject &request, QString *error) const {
    try {
        if (!m_drive.isReady()) throw std::runtime_error("The Society drive is not ready.");
        if (!std::filesystem::exists(FileRepository::packagePath(detail::nativePath(m_drive.rootPath()), utf8(m_drive.identifier())))) return {{"files", QJsonArray{}}, {"pending", true}};
        auto repository = fileRepository({}, FileRepository::Access::ReadOnly);
        const auto limit = unsignedValue(request.value("limit"), 256);
        if (!limit || limit > 1024) throw std::runtime_error("Search limit must be 1 to 1024.");
        const auto cursor = request.value("cursor").toObject(); QJsonArray files; QJsonObject next;
        const bool semantic = request.contains("classKey") || request.contains("conditions") || request.contains("directory");
        if (semantic) {
            if (request.contains("directory") && (request.contains("classKey") || request.contains("conditions")))
                throw std::runtime_error("A saved semantic directory already owns its query.");
            if (request.contains("conditions") && !request.value("conditions").isArray()) throw std::runtime_error("Ontology conditions require an array.");
            if (request.contains("text") || request.contains("keywords") || request.contains("minimumRating") || request.contains("color") || request.contains("collections"))
                throw std::runtime_error("Use typed ontology conditions to combine semantic classification filters.");
            OntologyQuery query; query.classKey = utf8(request.value("classKey").toString(qtext(FileRepository::fileClass)));
            query.includeSubclasses = request.value("includeSubclasses").toBool(true);
            for (const auto &condition : request.value("conditions").toArray()) {
                if (!condition.isObject()) throw std::runtime_error("Invalid ontology condition.");
                const auto value = condition.toObject(); OntologyCondition entry; entry.propertyKey = stringValue(value.value("property"));
                if (value.contains("equals")) entry.equals = ontologyValue(value.value("equals").toObject()); query.conditions.push_back(entry);
            }
            OntologyCursor from;
            if (!cursor.isEmpty()) from = {unsignedValue(cursor.value("revision")), std::int64_t(unsignedValue(cursor.value("fileSequence"))),
                std::int64_t(unsignedValue(cursor.value("afterIndexKey"))), stringValue(cursor.value("fingerprint"))};
            const auto page = request.contains("directory") ? repository->ontology().listDirectory(stringValue(request.value("directory")), from, limit)
                : repository->ontology().queryFiles(query, from, limit);
            for (const auto &entry : page.files) {
                if (entry.path.starts_with(".originals/")) continue;
                const auto slash = entry.path.find('/'); if (slash == std::string::npos) continue;
                const auto key = qtext(entry.path.substr(0, slash));
                for (const auto section : m_drive.sections()) if (storeSectionKey(section) == key) {
                    const auto native = filePath(section, qtext(entry.path.substr(slash + 1)));
                    if (!native.isEmpty() && QFileInfo(native).isFile()) if (auto current = repository->lookup(entry.path)) files.append(row(*current));
                }
            }
            if (page.next) next = cursorJson(*page.next);
        } else {
            const auto section = utf8(request.value("section").toString("files"));
            FileFilter filter; filter.text = utf8(request.value("text").toString()); filter.pathPrefix = utf8(request.value("pathPrefix").toString());
            filter.allKeywords = strings(request.value("keywords")); filter.collections = strings(request.value("collections"));
            if (request.contains("minimumRating")) { const auto rating = unsignedValue(request.value("minimumRating")); if (rating > 5) throw std::runtime_error("Rating must be 0 to 5."); filter.minimumRating = unsigned(rating); }
            if (request.contains("color")) {
                constexpr std::array<const char *, 8> colors{"none","red","orange","yellow","green","blue","purple","gray"};
                const auto wanted = stringValue(request.value("color")); const auto it = std::ranges::find(colors, wanted);
                if (it == colors.end()) throw std::runtime_error("Unknown file color."); filter.colors = {static_cast<ColorLabel>(it - colors.begin())};
            }
            const auto fields = request.value("fields").toObject(); for (auto it = fields.begin(); it != fields.end(); ++it) filter.fields.emplace(utf8(it.key()), stringValue(it.value()));
            std::optional<FileCursor> from;
            if (!cursor.isEmpty()) from = FileCursor{stringValue(cursor.value("catalog")), unsignedValue(cursor.value("revision")),
                std::int64_t(unsignedValue(cursor.value("afterIndex"))), stringValue(cursor.value("filter"))};
            const auto page = repository->search(section, filter, from, limit);
            for (const auto &entry : page.files) if (auto current = repository->lookup(section + '/' + entry.path)) {
                auto value = row(*current); value.insert("title", qtext(entry.title)); value.insert("rating", int(entry.rating)); files.append(value);
            }
            if (page.next) next = cursorJson(*page.next);
        }
        if (error) error->clear(); return {{"files", files}, {"next", next}, {"provider", "iiFileProvider"}};
    } catch (const std::exception &e) { if (error) *error = QString::fromUtf8(e.what()); return {}; }
}
}
