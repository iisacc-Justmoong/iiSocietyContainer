#include <SocietyDrive.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

using namespace iiSocietyContainer;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    QFile output;
    output.open(stdout, QIODevice::WriteOnly);
    if (args.size() == 2 && args[1] == QStringLiteral("catalog")) {
        QJsonArray sections;
        for (const auto section : allStoreSections()) {
            sections.append(QJsonObject{{"id", storeSectionKey(section)},
                {"name", storeSectionName(section)}, {"path", storeSectionName(section)}});
        }
        output.write(QJsonDocument(sections).toJson());
        return 0;
    }
    if (args.size() != 3 || (args[1] != "create" && args[1] != "open")) {
        output.write("Usage: iiSocietyContainerDriveTool catalog | create <directory> | open <directory>\n");
        return 2;
    }
    QString error;
    const auto drive = args[1] == "create" ? SocietyDrive::create(args[2], &error)
                                          : SocietyDrive::open(args[2], &error);
    if (!drive) {
        output.write(QJsonDocument(QJsonObject{{"error", error}}).toJson());
        return 1;
    }
    output.write(QJsonDocument(QJsonObject{{"identifier", drive->identifier()},
        {"displayName", drive->displayName()}, {"rootPath", drive->rootPath()}}).toJson());
    return 0;
}
