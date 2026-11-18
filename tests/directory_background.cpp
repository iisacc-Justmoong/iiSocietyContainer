#include "src/Models/DirectoryBackground.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include "CTestSupport.h"
#include <QThread>
#include <memory>

class DirectoryBackgroundTests final {
public:
    void applicationWaitsForDetachedViewWork() {
        QTemporaryDir fixture(QDir::temp().filePath("directory-worker-XXXXXX")); CTEST_CHECK(fixture.isValid());
        const auto completed=fixture.filePath("completed"), callback=fixture.filePath("callback");
        QProcess child; child.start(QCoreApplication::applicationFilePath(),{"--child",completed,callback});
        CTEST_CHECK(child.waitForStarted()); CTEST_CHECK(child.waitForFinished(15000));
        CTEST_COMPARE(child.exitStatus(),QProcess::NormalExit); CTEST_COMPARE(child.exitCode(),0);
        CTEST_CHECK(QFile::exists(completed)); CTEST_CHECK(!QFile::exists(callback));
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
    society_ctest::CTestRunner runner;
    DirectoryBackgroundTests test;
    runner.run("applicationWaitsForDetachedViewWork", [&] { test.applicationWaitsForDetachedViewWork(); });
    return runner.result();
}
