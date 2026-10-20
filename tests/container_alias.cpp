#include <Container.h>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <QElapsedTimer>
#include <algorithm>
#include <array>

using namespace iiSocietyContainer;
class ContainerAliasTests : public QObject {
    Q_OBJECT
private slots:
    void stateIsAnAliasSnapshotAndDoesNotProbeOrCreateStorage() {
        Container container;
        QSignalSpy changed(&container, &Container::stateChanged);
        const QVariantMap png{{"id", "drive/generation-history/result.png"}, {"containerId", "drive"},
            {"key", "generation-history/result.png"}, {"section", "generation-history"},
            {"name", "result.png"}, {"exists", true}, {"available", false}, {"isImage", true},
            {"previewSource", QUrl("https://example.invalid/preview.png")}};
        container.synchronize("drive", "/unmounted/society", {png}, true);
        QVERIFY(container.available()); QCOMPARE(container.aliases().size(), 1);
        QCOMPARE(container.alias("generation-history/result.png")["exists"].toBool(), true);
        QCOMPARE(container.alias("generation-history/result.png")["available"].toBool(), false);
        QCOMPARE(container.generationHistory().first().toMap()["previewSource"].toUrl(), QUrl("https://example.invalid/preview.png"));
        container.synchronize("drive", "/unmounted/society", {}, true);
        QVERIFY(container.alias("generation-history/result.png").isEmpty());
        QCOMPARE(changed.size(), 2);
        container.synchronize({}, {}, {}, false, "disconnected");
        QVERIFY(!container.available()); QVERIFY(container.aliases().isEmpty());
        QCOMPARE(container.errorString(), "disconnected");
    }
    void aliasesFromAnotherContainerAreNeverAccepted() {
        Container container;
        container.synchronize("new", "/new", {QVariantMap{{"containerId", "old"}, {"key", "files/document.iisc"}}}, true);
        QVERIFY(container.aliases().isEmpty());
    }
    void lookupFollowsReplacementFilteringAndReentrantReaders() {
        Container first, second;
        const QVariantMap a{{"containerId","one"},{"key","files/a"},{"name","A"},{"exists",true}};
        auto b=a;b["key"]="files/b";b["name"]="B";
        auto duplicate=a;duplicate["name"]="ignored duplicate";
        auto foreign=a;foreign["containerId"]="foreign";foreign["key"]="files/foreign";
        auto invalid=a;invalid["key"]="files/../escape";
        first.synchronize("one","/one",{b,a,duplicate,foreign,invalid},true);
        QCOMPARE(first.aliases().size(),2);QCOMPARE(first.alias("files/a")["name"].toString(),"A");
        QVERIFY(first.alias("files/foreign").isEmpty());QVERIFY(first.alias("files/../escape").isEmpty());
        auto other=a;other["containerId"]="two";other["name"]="Other owner";
        second.synchronize("two","/two",{other},true);
        QCOMPARE(second.alias("files/a")["name"].toString(),"Other owner");
        QSignalSpy changed(&first,&Container::stateChanged);
        auto replacement=b;replacement["name"]="Replaced";
        bool readDuringNotification=false;
        connect(&first,&Container::stateChanged,&first,[&] {
            readDuringNotification=first.alias("files/b")["name"]=="Replaced" && first.alias("files/a").isEmpty();
        });
        first.synchronize("one","/one",{replacement},true);QVERIFY(readDuringNotification);
        const auto revision=first.revision();first.setQuery("unrelated Home query");
        QCOMPARE(first.revision(),revision);QCOMPARE(first.alias("files/b")["name"].toString(),"Replaced");
        const auto notifications=changed.size();
        auto returned=first.alias("files/b");returned["name"]="Caller edit";
        QCOMPARE(first.alias("files/b")["name"].toString(),"Replaced");QCOMPARE(changed.size(),notifications);
        first.synchronize({}, {}, {a}, false);QVERIFY(first.alias("files/b").isEmpty());
        first.synchronize("one","/one",{a},true);QCOMPARE(first.alias("files/a")["name"].toString(),"A");
    }
    void repeatedLookupCostIsMeasuredWithoutStorageWork() {
        constexpr int count=10000,queries=2000;Container container;QVariantList rows;QStringList keys;
        for(int i=0;i<count;++i) {
            const auto key="files/"+QString::number(i);keys.append(key);
            rows.append(QVariantMap{{"key",key},{"containerId","benchmark"},{"ordinal",i},{"exists",true}});
        }
        container.synchronize("benchmark","/not-a-real-drive",rows,true);
        QSignalSpy changed(&container,&Container::stateChanged);std::array<qint64,3> times;
        for(auto &time:times) {
            QElapsedTimer clock;clock.start();
            for(int i=0;i<queries;++i) {const auto n=i*7919%count;QCOMPARE(container.alias(keys[n])["ordinal"].toInt(),n);}
            time=clock.nsecsElapsed()/1000;
        }
        std::sort(times.begin(),times.end());
        qInfo()<<"PERF alias_lookup rows="<<count<<"queries="<<queries<<"median_us="<<times[1]<<"sizeof_Container="<<sizeof(Container);
        QCOMPARE(changed.size(),0);QVERIFY(container.alias("files/missing").isEmpty());
    }
};
QTEST_GUILESS_MAIN(ContainerAliasTests)
#include "container_alias.moc"
