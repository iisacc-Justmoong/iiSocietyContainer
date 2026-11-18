#include <Container.h>
#include "CTestSupport.h"
#include <QCoreApplication>
#include <QUrl>
#include <QElapsedTimer>
#include <algorithm>
#include <array>

using namespace iiSocietyContainer;
class ContainerAliasTests {
public:
    void repeatedSnapshotsKeepRevisionAndChangedInputsAreStillValidated() {
        Container container; QVariantList rows;
        for (int i=0; i<2000; ++i) rows.append(QVariantMap{{"containerId","perf"},
            {"key",QString("files/%1").arg(i,5,10,QChar('0'))}, {"modified","2026-10-21T00:00:00Z"}});
        QElapsedTimer timer; timer.start();
        container.synchronize("perf","/not-a-real-drive",rows,true);
        const auto first=timer.nsecsElapsed(); const auto revision=container.revision();
        int changedCount = 0; QObject signalContext;
        QObject::connect(&container, &Container::stateChanged, &signalContext, [&] { ++changedCount; });
        timer.restart();
        for (int i=0;i<5;++i) container.synchronize("perf","/not-a-real-drive",rows,true);
        qInfo("PERF snapshot 2000 first_us=%lld repeated_mean_us=%lld",first/1000,timer.nsecsElapsed()/5000);
        CTEST_COMPARE(changedCount,0); CTEST_COMPARE(container.revision(),revision);
        std::reverse(rows.begin(),rows.end());
        container.synchronize("perf","/not-a-real-drive",rows,true);
        CTEST_COMPARE(changedCount,0); CTEST_COMPARE(container.revision(),revision);
        auto modified=rows[0].toMap(); modified["key"]="files/../escape"; rows[0]=modified;
        container.synchronize("perf","/not-a-real-drive",rows,true);
        CTEST_COMPARE(container.aliases().size(),1999); CTEST_COMPARE(changedCount,1);
        container.synchronize("other","/other",rows,true);
        CTEST_CHECK(container.aliases().isEmpty());
    }
    void stateIsAnAliasSnapshotAndDoesNotProbeOrCreateStorage() {
        Container container;
        int changedCount = 0; QObject signalContext;
        QObject::connect(&container, &Container::stateChanged, &signalContext, [&] { ++changedCount; });
        const QVariantMap png{{"id", "drive/generation-history/result.png"}, {"containerId", "drive"},
            {"key", "generation-history/result.png"}, {"section", "generation-history"},
            {"name", "result.png"}, {"exists", true}, {"available", false}, {"isImage", true},
            {"previewSource", QUrl("https://example.invalid/preview.png")}};
        container.synchronize("drive", "/unmounted/society", {png}, true);
        CTEST_CHECK(container.available()); CTEST_COMPARE(container.aliases().size(), 1);
        CTEST_COMPARE(container.alias("generation-history/result.png")["exists"].toBool(), true);
        CTEST_COMPARE(container.alias("generation-history/result.png")["available"].toBool(), false);
        CTEST_COMPARE(container.generationHistory().first().toMap()["previewSource"].toUrl(), QUrl("https://example.invalid/preview.png"));
        container.synchronize("drive", "/unmounted/society", {}, true);
        CTEST_CHECK(container.alias("generation-history/result.png").isEmpty());
        CTEST_COMPARE(changedCount, 2);
        container.synchronize({}, {}, {}, false, "disconnected");
        CTEST_CHECK(!container.available()); CTEST_CHECK(container.aliases().isEmpty());
        CTEST_COMPARE(container.errorString(), "disconnected");
    }
    void aliasesFromAnotherContainerAreNeverAccepted() {
        Container container;
        container.synchronize("new", "/new", {QVariantMap{{"containerId", "old"}, {"key", "files/document.iisc"}}}, true);
        CTEST_CHECK(container.aliases().isEmpty());
    }
    void lookupFollowsReplacementFilteringAndReentrantReaders() {
        Container first, second;
        const QVariantMap a{{"containerId","one"},{"key","files/a"},{"name","A"},{"exists",true}};
        auto b=a;b["key"]="files/b";b["name"]="B";
        auto duplicate=a;duplicate["name"]="ignored duplicate";
        auto foreign=a;foreign["containerId"]="foreign";foreign["key"]="files/foreign";
        auto invalid=a;invalid["key"]="files/../escape";
        first.synchronize("one","/one",{b,a,duplicate,foreign,invalid},true);
        CTEST_COMPARE(first.aliases().size(),2);CTEST_COMPARE(first.alias("files/a")["name"].toString(),"A");
        CTEST_CHECK(first.alias("files/foreign").isEmpty());CTEST_CHECK(first.alias("files/../escape").isEmpty());
        auto other=a;other["containerId"]="two";other["name"]="Other owner";
        second.synchronize("two","/two",{other},true);
        CTEST_COMPARE(second.alias("files/a")["name"].toString(),"Other owner");
        int changedCount = 0; bool readDuringNotification=false; QObject signalContext;
        QObject::connect(&first,&Container::stateChanged,&signalContext,[&] { ++changedCount; });
        auto replacement=b;replacement["name"]="Replaced";
        QObject::connect(&first,&Container::stateChanged,&signalContext,[&] {
            readDuringNotification=first.alias("files/b")["name"]=="Replaced" && first.alias("files/a").isEmpty();
        });
        first.synchronize("one","/one",{replacement},true);CTEST_CHECK(readDuringNotification);
        const auto revision=first.revision();first.setQuery("unrelated Home query");
        CTEST_COMPARE(first.revision(),revision);CTEST_COMPARE(first.alias("files/b")["name"].toString(),"Replaced");
        const auto notifications=changedCount;
        auto returned=first.alias("files/b");returned["name"]="Caller edit";
        CTEST_COMPARE(first.alias("files/b")["name"].toString(),"Replaced");CTEST_COMPARE(changedCount,notifications);
        first.synchronize({}, {}, {a}, false);CTEST_CHECK(first.alias("files/b").isEmpty());
        first.synchronize("one","/one",{a},true);CTEST_COMPARE(first.alias("files/a")["name"].toString(),"A");
    }
    void repeatedLookupCostIsMeasuredWithoutStorageWork() {
        constexpr int count=10000,queries=2000;Container container;QVariantList rows;QStringList keys;
        for(int i=0;i<count;++i) {
            const auto key="files/"+QString::number(i);keys.append(key);
            rows.append(QVariantMap{{"key",key},{"containerId","benchmark"},{"ordinal",i},{"exists",true}});
        }
        container.synchronize("benchmark","/not-a-real-drive",rows,true);
        int changedCount = 0; QObject signalContext;
        QObject::connect(&container, &Container::stateChanged, &signalContext, [&] { ++changedCount; });std::array<qint64,3> times;
        for(auto &time:times) {
            QElapsedTimer clock;clock.start();
            for(int i=0;i<queries;++i) {const auto n=i*7919%count;CTEST_COMPARE(container.alias(keys[n])["ordinal"].toInt(),n);}
            time=clock.nsecsElapsed()/1000;
        }
        std::sort(times.begin(),times.end());
        qInfo()<<"PERF alias_lookup rows="<<count<<"queries="<<queries<<"median_us="<<times[1]<<"sizeof_Container="<<sizeof(Container);
        CTEST_COMPARE(changedCount,0);CTEST_CHECK(container.alias("files/missing").isEmpty());
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    society_ctest::CTestRunner runner;
    ContainerAliasTests tests;
    runner.run("repeatedSnapshotsKeepRevisionAndChangedInputsAreStillValidated", [&] { tests.repeatedSnapshotsKeepRevisionAndChangedInputsAreStillValidated(); });
    runner.run("stateIsAnAliasSnapshotAndDoesNotProbeOrCreateStorage", [&] { tests.stateIsAnAliasSnapshotAndDoesNotProbeOrCreateStorage(); });
    runner.run("aliasesFromAnotherContainerAreNeverAccepted", [&] { tests.aliasesFromAnotherContainerAreNeverAccepted(); });
    runner.run("lookupFollowsReplacementFilteringAndReentrantReaders", [&] { tests.lookupFollowsReplacementFilteringAndReentrantReaders(); });
    runner.run("repeatedLookupCostIsMeasuredWithoutStorageWork", [&] { tests.repeatedLookupCostIsMeasuredWithoutStorageWork(); });
    return runner.result();
}
