#include "NativeMount.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <csignal>
#include <map>

using namespace iiSocietyContainer;
namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
QString stateDirectory()
{
    const auto override = qEnvironmentVariable("SOCIETY_MOUNT_STATE_DIRECTORY");
    return override.isEmpty() ? QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath("iisacc/Society/mounts")
                              : (QDir::isAbsolutePath(override) ? QDir::cleanPath(override) : QString());
}
QString serverName(const QString &directory)
{
    // Darwin's sockaddr_un is only 104 bytes, including Qt's temp directory.
    return "iisc-" + QString::fromLatin1(QCryptographicHash::hash(directory.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
}
QJsonObject failure(const QString &message) { return {{"error", message}}; }

class MountService {
public:
    struct Registration { QString root; QString point; std::unique_ptr<NativeMount> mount; QString error; };
    QString directory;
    std::map<QString, Registration> drives;
    explicit MountService(QString state) : directory(std::move(state)) {}
    QString registryPath() const { return QDir(directory).filePath("drives.json"); }
    bool save(const QString &excluding = {})
    {
        QJsonArray registrations;
        for (const auto &[id, entry] : drives)
            if (id != excluding) registrations.append(QJsonObject{{"identifier", id}, {"sourcePath", entry.root}, {"mountPoint", entry.point}});
        const auto bytes = QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"drives", registrations}}).toJson();
        QSaveFile output(registryPath());
        output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
    }
    void restore()
    {
        QFile input(registryPath());
        if (QFileInfo(input).isSymLink() || !input.open(QIODevice::ReadOnly) || input.size() > 65536) return;
        const auto registry = QJsonDocument::fromJson(input.readAll()).object();
        if (registry.value("schemaVersion").toInt() != 1) return;
        for (const auto &value : registry.value("drives").toArray()) {
            const auto row = value.toObject();
            const auto id = row.value("identifier").toString();
            const auto root = row.value("sourcePath").toString();
            auto view = FilesView::open(root);
            if (!view || view->drive().identifier() != id || drives.contains(id)) continue;
            Registration entry{root, row.value("mountPoint").toString(), {}, {}};
            entry.mount = mountFiles(*view, entry.point, &entry.error);
            if (!entry.mount) {
                entry.point = defaultMountPoint(id);
                entry.mount = mountFiles(*view, entry.point, &entry.error);
            }
            drives.emplace(id, std::move(entry));
        }
    }
    QJsonObject describe(const QString &id, Registration &entry)
    {
        auto view = FilesView::open(entry.root);
        if (!view || view->drive().identifier() != id) {
            entry.mount.reset();
            return failure("The original Society container is unavailable or was replaced.");
        }
        if (!entry.mount || !entry.mount->isRunning())
            return failure(entry.error.isEmpty() ? "Society Files is not mounted. Reconnect the container." : entry.error);
        return {{"identifier", id}, {"sourcePath", entry.root}, {"systemPath", entry.mount->path()}, {"enabled", true}};
    }
    QJsonObject request(const QJsonObject &request)
    {
        const auto action = request.value("action").toString();
        const auto argument = request.value("argument").toString();
        if (action == "register") {
            QString error;
            auto view = FilesView::open(argument, &error);
            if (!view) return failure(error);
            const auto id = view->drive().identifier();
            auto found = drives.find(id);
            if (found != drives.end()) {
                if (found->second.root != view->drive().rootPath()) return failure("This drive identity is already registered at another source path.");
                if (found->second.mount && found->second.mount->isRunning()) return describe(id, found->second);
                found->second.mount.reset();
            }
            Registration entry{view->drive().rootPath(), defaultMountPoint(id), {}, {}};
            if (entry.point.isEmpty()) return failure("No mount point is available.");
            entry.mount = mountFiles(*view, entry.point, &entry.error);
            if (!entry.mount) return failure(entry.error);
            drives.insert_or_assign(id, std::move(entry));
            if (!save()) { drives.erase(id); return failure("Could not persist the Society Files registration."); }
            auto result = describe(id, drives.at(id));
            if (qEnvironmentVariable("SOCIETY_MOUNT_AUTOSTART") != "0"
                && !installMountAutostart(QCoreApplication::applicationFilePath(), &error)) result.insert("warning", error);
            return result;
        }
        if (action == "list") {
            QJsonArray result;
            for (auto &[id, entry] : drives) result.append(describe(id, entry));
            return {{"drives", result}};
        }
        const auto found = drives.find(argument);
        if (found == drives.end()) return failure("Connect this Society container before opening its system drive.");
        if (action == "unregister") {
            if (!save(argument)) return failure("Could not remove the Society Files registration.");
            drives.erase(found);
            return {{"identifier", argument}, {"enabled", false}};
        }
        if (action == "path" || action == "refresh") return describe(argument, found->second);
        return failure("Unknown Society Files request.");
    }
};

int serve(const QString &directory)
{
    QDir().mkpath(directory);
    QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QLockFile lock(QDir(directory).filePath("service.lock"));
    if (!lock.tryLock(0)) return 0;
    const auto name = serverName(directory);
    QLocalServer::removeServer(name);
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(name)) { qCritical("Society Files: %s", qPrintable(server.errorString())); return 1; }
    MountService service(directory);
    service.restore();
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) {
            auto *socket = server.nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket, buffer, &service] {
                buffer->append(socket->readAll());
                if (buffer->size() > 65536) { socket->abort(); return; }
                if (!buffer->endsWith('\n')) return;
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(*buffer, &error);
                const auto response = error.error == QJsonParseError::NoError && document.isObject()
                    ? service.request(document.object()) : failure("Invalid Society Files request.");
                socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
                socket->disconnectFromServer();
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(15000, socket, [socket] { if (socket->state() != QLocalSocket::UnconnectedState) socket->abort(); });
        }
    });
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    QTimer shutdown;
    QObject::connect(&shutdown, &QTimer::timeout, &server, [&] {
        if (stopping) QCoreApplication::quit();
        for (auto &[id, entry] : service.drives)
            if (entry.mount) service.describe(id, entry);
    });
    shutdown.start(500);
    return QCoreApplication::exec();
}
QJsonObject call(const QString &directory, const QString &action, const QString &argument)
{
    QLocalSocket socket;
    const auto name = serverName(directory);
    socket.connectToServer(name);
    if (!socket.waitForConnected(1000)) {
        QProcess daemon;
        daemon.setProgram(QCoreApplication::applicationFilePath());
        daemon.setArguments({"serve"});
        daemon.setStandardOutputFile(QProcess::nullDevice());
        daemon.setStandardErrorFile(QProcess::nullDevice());
        if (!daemon.startDetached()) return failure("Could not start the Society Files service.");
        QElapsedTimer elapsed; elapsed.start();
        while (elapsed.elapsed() < 10000) {
            socket.abort(); socket.connectToServer(name);
            if (socket.waitForConnected(100)) break;
            QThread::msleep(50);
        }
    }
    if (socket.state() != QLocalSocket::ConnectedState) return failure("Society Files did not start.");
    socket.write(QJsonDocument(QJsonObject{{"action", action}, {"argument", argument}}).toJson(QJsonDocument::Compact) + '\n');
    if (socket.bytesToWrite() && !socket.waitForBytesWritten(5000)) return failure("Society Files did not accept the request.");
    QByteArray output;
    QElapsedTimer elapsed; elapsed.start();
    while (!output.endsWith('\n') && elapsed.elapsed() < 20000) {
        socket.waitForReadyRead(500);
        output += socket.readAll();
        if (output.size() > 65536) return failure("Invalid Society Files response.");
        if (socket.state() == QLocalSocket::UnconnectedState) break;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(output, &error);
    return error.error == QJsonParseError::NoError && document.isObject() ? document.object() : failure("Society Files did not respond.");
}
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const auto directory = stateDirectory();
    if (directory.isEmpty()) return 2;
    if (args.size() == 2 && args[1] == "serve") return serve(directory);
    QFile output; output.open(stdout, QIODevice::WriteOnly);
    if (args.size() < 2 || args.size() > 3 || !QStringList{"register", "path", "refresh", "unregister", "list"}.contains(args[1])) {
        output.write("Usage: iiSocietyContainerMount register <source> | path|refresh|unregister <id> | list\n");
        return 2;
    }
    const auto response = call(directory, args[1], args.value(2));
    output.write(QJsonDocument(response).toJson());
    return response.contains("error") ? 1 : 0;
}
