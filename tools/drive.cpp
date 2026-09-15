#include <SocietyDrive.h>
#include <ModelStore.h>

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
    if (args.size() == 2 && args[1] == "model-types") {
        QJsonArray types;
        for (const auto type : allModelTypes()) types.append(modelTypeName(type));
        output.write(QJsonDocument(types).toJson()); return 0;
    }
    if (args.size() == 3 && (args[1] == "models" || args[1] == "organize-models")) {
        QString error;
        const auto store = ModelStore::open(args[2], &error);
        if (!store) { output.write(QJsonDocument(QJsonObject{{"error", error}}).toJson()); return 1; }
        if (args[1] == "organize-models") {
            const auto report = store->organize();
            QJsonArray moved;
            for (const auto &move : report.moved) moved.append(QJsonObject{{"from", move.previousPath}, {"path", move.path}, {"type", modelTypeName(move.type)}});
            output.write(QJsonDocument(QJsonObject{{"moved", moved}, {"errors", QJsonArray::fromStringList(report.errors)}}).toJson());
            return report.errors.isEmpty() ? 0 : 1;
        }
        QJsonArray models;
        for (const auto &model : store->entries(&error)) models.append(QJsonObject{{"path", model.path}, {"relativePath", model.relativePath},
            {"type", modelTypeName(model.classification.type)}, {"recognized", model.classification.recognized}, {"evidence", model.classification.evidence}});
        output.write(QJsonDocument(QJsonObject{{"models", models}, {"error", error}}).toJson()); return error.isEmpty() ? 0 : 1;
    }
    if (args.size() != 3 || (args[1] != "create" && args[1] != "open")) {
        output.write("Usage: iiSocietyContainerDriveTool catalog | model-types | create <directory> | open <directory> | models <directory> | organize-models <directory>\n");
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
