#include "src/Models/DirectoryBackground.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <memory>

class DirectoryBackgroundTests final : public QObject {
    Q_OBJECT
private slots:
    void applicationWaitsForDetachedViewWork() {
        QTemporaryDir fixture(QDir::current().filePath("directory-worker-XXXXXX")); QVERIFY(fixture.isValid());
        const auto completed=fixture.filePath("completed"), callback=fixture.filePath("callback");
        QProcess child; child.start(QCoreApplication::applicationFilePath(),{"--child",completed,callback});
        QVERIFY(child.waitForStarted()); QVERIFY(child.waitForFinished(15000));
        QCOMPARE(child.exitStatus(),QProcess::NormalExit); QCOMPARE(child.exitCode(),0);
        QVERIFY(QFile::exists(completed)); QVERIFY(!QFile::exists(callback));
    }
};

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    const auto args=app.arguments();
    if (args.size()==4 && args.at(1)=="--child") {
        const auto completed=args.at(2), callback=args.at(3);
        auto owner=std::make_unique<QObject>();
        iiSocietyContainer::detail::directoryBackground(owner.get(),[completed] {
            QThread::msleep(300); QFile file(completed);
            return file.open(QIODevice::WriteOnly) && file.write("done")==4;
        },[callback](bool) { QFile file(callback); if(file.open(QIODevice::WriteOnly))file.write("unexpected"); });
        owner.reset();
        return 0;
    }
    DirectoryBackgroundTests test; return QTest::qExec(&test,argc,argv);
}
#include "directory_background.moc"
