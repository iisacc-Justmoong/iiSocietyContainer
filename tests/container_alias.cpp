#include <Container.h>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>

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
};
QTEST_GUILESS_MAIN(ContainerAliasTests)
#include "container_alias.moc"
