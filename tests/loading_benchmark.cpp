#include <StorageDirectoryModel.h>
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
    qputenv("IISOCIETY_FILE_CACHE_DIRECTORY", QDir::current().filePath("loading-cache").toUtf8());
    const auto folder = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::current().filePath("loading-fixture");
    if (argc == 1) {
        QDir().mkpath(folder);
        for (int i = 0; i < 2000; ++i) {
            QFile file(QDir(folder).filePath(QString("file-%1.txt").arg(i, 5, 10, QChar('0'))));
            if (!file.exists() && file.open(QIODevice::WriteOnly)) file.write("Society loading benchmark\n");
        }
    }
    QJsonArray runs;
    for (int i = 0; i < 3; ++i) {
        iiSocietyContainer::StorageDirectoryModel model;
        QEventLoop loop; QElapsedTimer timer; timer.start();
        qint64 firstRows = -1;
        QObject::connect(&model, &iiSocietyContainer::StorageDirectoryModel::countChanged, &loop, [&] {
            if (firstRows < 0 && model.rowCount()) firstRows = timer.elapsed();
        });
        QObject::connect(&model, &iiSocietyContainer::StorageDirectoryModel::statusChanged, &loop, [&] {
            if (model.status() == iiSocietyContainer::StorageDirectoryModel::Ready) loop.quit();
        });
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        model.setFolder(QUrl::fromLocalFile(folder)); loop.exec();
        if (model.status() != iiSocietyContainer::StorageDirectoryModel::Ready) return 2;
        runs.append(QJsonObject{{"run", i + 1}, {"readyMs", timer.elapsed()}, {"firstRowsMs", firstRows}, {"rows", model.rowCount()}});
    }
    std::printf("%s\n", QJsonDocument(runs).toJson(QJsonDocument::Compact).constData());
}
