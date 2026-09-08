#include "AndroidStorage.h"
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QJsonDocument>
#include <QJsonObject>

namespace iiSocietyContainer {
bool androidIsStorageOwner()
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    return context.callObjectMethod("getPackageName", "()Ljava/lang/String;").toString() == "com.iisacc.society";
}
QByteArray androidSharedRequest(const QString &action, const QString &section, const QString &path, const QString &identifier)
{
    const auto result = QJniObject::callStaticObjectMethod("com/iisacc/society/storage/SocietyClient", "request",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        QNativeInterface::QAndroidApplication::context().object(), QJniObject::fromString(action).object(),
        QJniObject::fromString(section).object(), QJniObject::fromString(path).object(), QJniObject::fromString(identifier).object());
    return result.isValid() ? result.toString().toUtf8() : QByteArray("{\"error\":\"Package the Society Android client bridge\"}");
}
QByteArray androidDriveRequest(const QString &action, const QString &source, const QString &identifier)
{
    const auto context = QNativeInterface::QAndroidApplication::context();
    const auto result = QJniObject::callStaticObjectMethod("com/iisacc/society/storage/SocietyStorage", "request",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        context.object(), QJniObject::fromString(action).object(), QJniObject::fromString(source).object(),
        QJniObject::fromString(identifier).object());
    return result.isValid() ? result.toString().toUtf8() : QByteArray("{\"error\":\"The Android Society provider is not packaged\"}");
}
QString androidDocumentName(const QString &contentUri)
{
    return QJniObject::callStaticObjectMethod("com/iisacc/society/storage/SocietyStorage", "displayName",
        "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;",
        QNativeInterface::QAndroidApplication::context().object(), QJniObject::fromString(contentUri).object()).toString();
}
QString androidSharedStorageRoot(QString *error)
{
    const auto result = QJsonDocument::fromJson(androidDriveRequest(QStringLiteral("default"))).object();
    if (error) *error = result.value("error").toString();
    return result.value("sourcePath").toString();
}
}
