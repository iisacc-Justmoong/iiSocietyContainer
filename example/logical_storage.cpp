#include <ApplicationStorage.h>
#include <QCoreApplication>
#include <QJsonDocument>
#include <iiFileProviderDevice.h>
#include <iostream>

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    // A fixture-owned group keeps this executable independent of real app data.
    iiFileProvider::TemporaryDirectory group(QStringLiteral(LOGICAL_STORAGE_EXAMPLE_DIRECTORY "/logical-example-XXXXXX"));
    QString error;
    const auto created = iiSocietyContainer::ApplicationStorage::openInGroup(group.path(), "com.iisacc.dreamscapes", &error);
    if (!created) { std::cerr << error.toStdString(); return 1; }
    const auto another = iiSocietyContainer::ApplicationStorage::openInGroup(group.path(), "com.iisacc.another", &error);
    if (!another || another->drive().rootPath() != created->drive().rootPath()
        || another->drive().identifier() != created->drive().identifier()) return 3;
    const auto adopted = iiSocietyContainer::ApplicationStorage::adoptInGroup(group.path(), &error);
    if (!adopted || adopted->drive().identifier() != created->drive().identifier()
        || adopted->drive().rootPath() != created->drive().rootPath()) return 2;
    std::cout << QJsonDocument(iiSocietyContainer::ApplicationStorage::registration(group.path())).toJson().toStdString();
    return 0;
}
