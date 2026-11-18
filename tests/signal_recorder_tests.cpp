#include "CTestSupport.h"
#include "SignalRecorder.h"
#include <QCoreApplication>
#include <QTimer>
#include <QStringListModel>

struct CustomPayload { int number; QString text; };
Q_DECLARE_METATYPE(CustomPayload)

class SignalFixture final : public QObject {
    Q_OBJECT
signals:
    void event(QString name, bool accepted);
    void custom(CustomPayload payload);
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    society_ctest::CTestRunner runner;
    runner.run("capturesArgumentsAndWaitsForTheNextSignal", [] {
        SignalFixture sender;
        society_ctest::SignalRecorder records(&sender, &SignalFixture::event);
        emit sender.event("first", true);
        CTEST_COMPARE(records.size(), 1);
        CTEST_COMPARE(records[0][0].toString(), QString("first"));
        CTEST_CHECK(records[0][1].toBool());
        QTimer::singleShot(0, [&] { emit sender.event("second", false); });
        CTEST_CHECK(records.wait(1000));
        CTEST_COMPARE(records.size(), 2);
        CTEST_COMPARE(records[1][0].toString(), QString("second"));
        CTEST_COMPARE(records.first()[0].toString(), QString("first"));
        CTEST_COMPARE(records.last()[0].toString(), QString("second"));
        CTEST_COMPARE(records.at(1)[0].toString(), QString("second"));
        CTEST_CHECK(!records[1][1].toBool());
        CTEST_CHECK(!records.wait(0));
    });
    runner.run("preservesCustomPayload", [] {
        SignalFixture sender;
        society_ctest::SignalRecorder records(&sender, &SignalFixture::custom);
        emit sender.custom({42, QStringLiteral("payload")});
        CTEST_COMPARE(records.size(), 1);
        const auto payload = qvariant_cast<CustomPayload>(records[0][0]);
        CTEST_COMPARE(payload.number, 42);
        CTEST_COMPARE(payload.text, QStringLiteral("payload"));
    });
    runner.run("disconnectsWhenRecorderIsDestroyed", [] {
        SignalFixture sender;
        {
            society_ctest::SignalRecorder records(&sender, &SignalFixture::event);
            emit sender.event("first", true);
            CTEST_COMPARE(records.count(), 1);
        }
        emit sender.event("after", true);
    });
    runner.run("countsModelPrivateSignals", [] {
        QStringListModel model;
        society_ctest::SignalCounter inserted(&model, &QAbstractItemModel::rowsInserted);
        society_ctest::SignalCounter reset(&model, &QAbstractItemModel::modelReset);
        CTEST_CHECK(model.insertRows(0, 1)); CTEST_COMPARE(inserted.count(), 1);
        model.setStringList({"replacement"}); CTEST_COMPARE(reset.count(), 1);
    });
    runner.run("rejectsMissingSender", [] {
        SignalFixture* sender = nullptr;
        CTEST_THROWS(std::runtime_error, society_ctest::SignalRecorder(sender, &SignalFixture::event));
    });
    return runner.result();
}
#include "signal_recorder_tests.moc"
