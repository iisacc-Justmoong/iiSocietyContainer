#include <StorageDirectoryModel.h>
#include <SocietyDrive.h>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <cstdio>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); app.setApplicationName("SocietyLoadingBenchmark");
    const auto fixture = QDir::current().filePath("loading-fixture.societycontainer");
    const auto folder = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir(fixture).filePath("Files");
    if (argc == 1) {
        qunsetenv("IISOCIETY_FILE_CACHE_DIRECTORY");
        QDir().mkpath(fixture);
        if (!iiSocietyContainer::SocietyDrive::open(fixture)
            && !iiSocietyContainer::SocietyDrive::create(fixture)) return 3;
        for (int i = 0; i < 2000; ++i) {
            QFile file(QDir(folder).filePath(QString("file-%1.txt").arg(i, 5, 10, QChar('0'))));
            if (!file.exists() && file.open(QIODevice::WriteOnly)) file.write("Society loading benchmark\n");
        }
    } else qputenv("IISOCIETY_FILE_CACHE_DIRECTORY", QDir::current().filePath("loading-cache-disabled").toUtf8());
    QJsonArray runs;
    for (int i = 0; i < 3; ++i) {
        iiSocietyContainer::StorageDirectoryModel model;
        QEventLoop loop; QElapsedTimer timer; timer.start();
        qint64 firstRows = -1;
        bool snapshot = false;
        QObject::connect(&model, &iiSocietyContainer::StorageDirectoryModel::snapshotRestored, &loop, [&] { snapshot = true; });
        QObject::connect(&model, &iiSocietyContainer::StorageDirectoryModel::countChanged, &loop, [&] {
            if (firstRows < 0 && model.rowCount()) firstRows = timer.elapsed();
        });
        QObject::connect(&model, &iiSocietyContainer::StorageDirectoryModel::statusChanged, &loop, [&] {
            if (model.status() == iiSocietyContainer::StorageDirectoryModel::Ready) loop.quit();
        });
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        model.setFolder(QUrl::fromLocalFile(folder)); loop.exec();
        if (model.status() != iiSocietyContainer::StorageDirectoryModel::Ready) return 2;
        const auto ready = timer.elapsed();
        timer.restart();
        qsizetype fields = 0;
        for (int row = 0; row < model.rowCount(); ++row) fields += model.entry(row).size();
        runs.append(QJsonObject{{"run", i + 1}, {"readyMs", ready}, {"firstRowsMs", firstRows},
            {"rows", model.rowCount()}, {"snapshotRestored", snapshot}, {"rowMapsMs", timer.elapsed()}, {"fields", fields}});
        // Snapshot persistence is asynchronous and intentionally outside Ready.
        if (argc == 1) { QTimer::singleShot(500, &loop, &QEventLoop::quit); loop.exec(); }
    }
    std::printf("%s\n", QJsonDocument(runs).toJson(QJsonDocument::Compact).constData());
}
