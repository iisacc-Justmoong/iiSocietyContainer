#include "StorageMap.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <QRegularExpression>

namespace iiSocietyContainer {
namespace {
bool ordinary(const QString &path) {
    const QFileInfo info(path);
    return !info.isSymLink() && !info.isJunction()
        && (!info.exists() || info.canonicalFilePath() == path);
}
bool validId(const QString &id) {
    return !QUuid(id).isNull() && QUuid(id).toString(QUuid::WithoutBraces) == id;
}
void fail(QString *error, const QString &text) { if (error) *error = text; }
}
StorageMap::StorageMap(const SocietyDrive &drive) : m_drive(drive) {}
QString StorageMap::physicalPath(const QString &key) {
    if (!key.contains('/') || key.size() > 3500 || key.contains('\\') || key.contains(':') || key.contains(QChar::Null)) return {};
    for (const auto &part : key.split('/'))
        if (part.isEmpty() || part == "." || part == ".." || part.startsWith(".society-") || part.startsWith(".iiserverhost-")) return {};
    for (const auto section : allStoreSections())
        if (key.section('/', 0, 0) == storeSectionKey(section)) return storeSectionName(section) + '/' + key.section('/', 1);
    return {};
}
QString StorageMap::logicalPath(const QString &relative) {
    for (const auto section : allStoreSections()) {
        const auto prefix = storeSectionName(section) + '/';
        if (relative.startsWith(prefix)) {
            const auto key = storeSectionKey(section) + '/' + relative.mid(prefix.size());
            return physicalPath(key).isEmpty() ? QString() : key;
        }
    }
    return {};
}
bool StorageMap::prepare(QString *error) const {
    if (!m_drive.isValid()) { fail(error, "The Society container is unavailable."); return false; }
    auto path = m_drive.rootPath();
    for (const auto *part : {".society-sync", "requests"}) {
        path += '/' + QString::fromLatin1(part);
        if (!ordinary(path) || (!QFileInfo::exists(path) && !QDir().mkdir(path)) || !QFileInfo(path).isDir()) {
            fail(error, "The Society storage map is unavailable or redirected."); return false;
        }
    }
    return true;
}
QJsonObject StorageMap::read(const QString &relative, qint64 limit) const {
    if (!m_drive.isValid()) return {};
    QString path = m_drive.rootPath();
    for (const auto &part : relative.split('/')) { path += '/' + part; if (!ordinary(path)) return {}; }
    QFile input(path);
    if (input.size() > limit || !input.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(input.readAll()).object();
}
bool StorageMap::write(const QString &relative, const QJsonObject &value, QString *error) const {
    if (!prepare(error)) return false;
    const auto path = QDir(m_drive.rootPath()).filePath(".society-sync/" + relative);
    if (!ordinary(path)) { fail(error, "The Society metadata path is redirected."); return false; }
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    QFile old(path);
    if (old.open(QIODevice::ReadOnly) && old.size() == bytes.size() && old.readAll() == bytes) return true;
    old.close(); QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || !output.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        || output.write(bytes) != bytes.size() || !output.commit()) { fail(error, output.errorString()); return false; }
    return true;
}
QJsonArray StorageMap::objects(QString *error) const {
    const auto snapshot = read(".society-sync/catalog.json", 128 * 1024 * 1024);
    if (snapshot.isEmpty()) return {};
    if (snapshot.value("schema") != 1 || snapshot.value("container") != m_drive.identifier()
        || !snapshot.value("objects").isArray()) { fail(error, "The Society storage map belongs to a different container."); return {}; }
    return snapshot.value("objects").toArray();
}
bool StorageMap::isLocalAuthority() const {
    const auto primary = read(".society-sync/primary.json", 8192);
    static const QRegularExpression scope("\\A[a-f0-9]{64}\\z");
    const auto host = primary.value("host").toString();
    return primary.value("schema") == 1 && primary.value("container") == m_drive.identifier()
        && !host.isEmpty() && host.size() <= 128 && scope.match(primary.value("scope").toString()).hasMatch();
}
QJsonObject StorageMap::object(const QString &key) const {
    for (const auto &value : objects()) if (value.toObject().value("path") == key) return value.toObject();
    return {};
}
QStringList StorageMap::files(const QString &key) const {
    QStringList result;
    if (physicalPath(key).isEmpty()) return result;
    for (const auto &value : objects()) {
        const auto e = value.toObject(); const auto path = e.value("path").toString();
        if (e.value("kind") == "file" && (path == key || path.startsWith(key + '/'))) result.append(path);
    }
    result.sort(); return result;
}
QString StorageMap::localPath(const QString &key) const {
    const auto relative = physicalPath(key); if (relative.isEmpty() || !m_drive.isValid()) return {};
    const auto parts = relative.split('/');
    auto path = m_drive.resolvePath(parts.first());
    if (path.isEmpty() || !ordinary(path)) return {};
    for (const auto &part : parts.mid(1)) { path += '/' + part; if (!ordinary(path)) return {}; }
    return path;
}
bool StorageMap::available(const QStringList &keys) const {
    QHash<QString, QJsonObject> entries;
    for (const auto &value : objects()) { const auto e = value.toObject(); entries.insert(e.value("path").toString(), e); }
    if (keys.isEmpty()) return false;
    for (const auto &key : keys) {
        if (!isResident(entries.value(key))) return false;
    }
    return true;
}
bool StorageMap::isResident(const QJsonObject &object) const {
    const auto path = localPath(object.value("path").toString());
    if (path.isEmpty() || object.value("kind") != "file" || !object.value("resident").toBool()) return false;
    const QFileInfo file(path);
    return file.isFile() && file.size() == object.value("size").toString().toLongLong();
}
QString StorageMap::previewPath(const QJsonObject &object) const {
    const auto hash = object.value("hash").toString();
    static const QRegularExpression hex("\\A[a-f0-9]{64}\\z");
    if (!hex.match(hash).hasMatch() || !m_drive.isValid()) return {};
    const auto directory = QDir(m_drive.rootPath()).filePath(".society-sync/previews");
    const auto path = directory + '/' + hash + ".jpg";
    return ordinary(QFileInfo(directory).absolutePath()) && ordinary(directory) && ordinary(path) && QFileInfo(path).isFile() ? path : QString();
}
QString StorageMap::request(const QStringList &keys, QString *error) const {
    QJsonArray requested; QHash<QString, QJsonObject> entries;
    for (const auto &value : objects()) { auto e = value.toObject(); entries.insert(e.value("path").toString(), e); }
    for (const auto &key : keys) {
        const auto e = entries.value(key);
        if (physicalPath(key).isEmpty() || e.value("kind") != "file") { fail(error, "The requested object is absent from the host storage map."); return {}; }
        requested.append(QJsonObject{{"path", key}, {"version", e.value("version")}});
    }
    if (requested.isEmpty() || requested.size() > 250000) { fail(error, "No files were selected for download."); return {}; }
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return write("requests/" + id + ".json", {{"id", id}, {"container", m_drive.identifier()}, {"state", "queued"}, {"objects", requested}}, error) ? id : QString();
}
QJsonObject StorageMap::requestState(const QString &id) const {
    if (!validId(id)) return {};
    const auto value = read(".society-sync/requests/" + id + ".json", 64 * 1024 * 1024);
    return value.value("id") == id && value.value("container") == m_drive.identifier() ? value : QJsonObject();
}
bool StorageMap::cancel(const QString &id) const {
    auto value = requestState(id); if (value.isEmpty()) return false;
    value["state"] = "cancelled"; return write("requests/" + id + ".json", value);
}
QJsonArray StorageMap::pendingRequests() const {
    QJsonArray result;
    const auto dir = QDir(m_drive.rootPath()).filePath(".society-sync/requests");
    if (!ordinary(QFileInfo(dir).absolutePath()) || !ordinary(dir)) return result;
    for (const auto &file : QDir(dir).entryList({"*.json"}, QDir::Files, QDir::Name)) {
        const auto value = requestState(file.chopped(5));
        if (value.value("state") == "queued") result.append(value);
    }
    return result;
}
bool StorageMap::finishRequest(const QString &id, const QString &error) const {
    auto value = requestState(id); if (value.value("state") != "queued") return false;
    value["state"] = error.isEmpty() ? "ready" : "failed"; value["error"] = error;
    return write("requests/" + id + ".json", value);
}
bool StorageMap::publish(const QJsonArray &objects, QString *error) const {
    return write("catalog.json", {{"schema", 1}, {"container", m_drive.identifier()}, {"objects", objects}}, error);
}
}
