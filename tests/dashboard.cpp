#include "SymbolicLink.h"
#include <iiSocietyContainer/DashboardFiles.h>
#include <iiSocietyContainer/SocietyApplication.h>
#include <SocietyDrive.h>
#include <SharedStorage.h>
#include <File.h>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileOpenEvent>
#include <QImage>
#include <QLocale>
#include <QSaveFile>
#include "SignalRecorder.h"
#include "QtEventWait.h"
#include <QGuiApplication>
#include <QEvent>
#include <QScopeGuard>
#include <QTemporaryDir>

using namespace iiSocietyContainer;

class DashboardTests : public QObject {
public:
    void providerMetadataAndOntologyAreTheActualSearchSource() {
        QTemporaryDir root(QDir::currentPath() + "/managed-dashboard-XXXXXX.societycontainer");
        CTEST_CHECK(SocietyDrive::create(root.path())); const auto storage = SharedStorage::open(root.path()); CTEST_CHECK(storage);
        iiFileProvider::File::create(root.filePath("Files/opaque.txt"), "original bytes");
        auto repository = storage->fileRepository(); iiFileProvider::FileMetadata metadata;
        metadata.title = "Launch campaign"; metadata.keywords = {"approved"}; metadata.rating = 5;
        const auto file = repository->observe("files/opaque.txt", metadata);
        using namespace iiFileProvider;
        repository->ontology().apply({OntologyClass{"ProjectAsset","Project","",{FileRepository::fileClass}},OntologyTypeAssignment{OntologyResource::file(file.key),"ProjectAsset"},OntologyDirectory{"Projects/Launch","Launch",{"ProjectAsset"}}},repository->ontology().revision(),repository->sessionKey());
        DashboardFiles model; model.setContainerPath(root.path()); CTEST_WAIT_DEFAULT(!model.loading());
        model.setQuery("campaign"); CTEST_WAIT_COMPARE_TIMEOUT(model.recentFiles().size(),1,30000);
        CTEST_COMPARE(model.recentFiles().front().toMap().value("name").toString(),"opaque.txt");
        model.setQuery("keyword:approved rating:5"); CTEST_WAIT_COMPARE_TIMEOUT(model.recentFiles().size(),1,30000);
        model.setQuery("class:ProjectAsset keyword:approved"); CTEST_WAIT_COMPARE_TIMEOUT(model.recentFiles().size(),1,30000);
        model.setQuery("in:Projects/Launch"); CTEST_WAIT_COMPARE_TIMEOUT(model.recentFiles().size(),1,30000);
        CTEST_CHECK_MESSAGE(model.errorString().isEmpty(),qPrintable(model.errorString()));
    }
    void documentsExposeRevisionedPreviewsWithoutBecomingImages()
    {
        QTemporaryDir root(QDir::currentPath() + "/document-dashboard-XXXXXX.societycontainer");
        CTEST_CHECK(SocietyDrive::create(root.path()));
        QFile canvas(root.filePath("Files/study.iisc"));
        CTEST_CHECK(canvas.open(QIODevice::WriteOnly)); canvas.write("native document"); canvas.close();
        DashboardFiles model; model.setContainerPath(root.path());
        CTEST_WAIT_DEFAULT(!model.loading());
        CTEST_COMPARE(model.recentFiles().size(), 1);
        const auto row = model.recentFiles().first().toMap();
        CTEST_CHECK(row.value("description").toString().contains("Canvas"));
        CTEST_COMPARE(row.value("thumbnailSource").toUrl().host(), "society-preview");
        CTEST_CHECK(row.value("previewSource").toUrl().isEmpty()); // Original-image contract is unchanged.
    }
    void providerControlDirectoryContentsNeverBecomeRecentFiles() {
        QTemporaryDir root(QDir::currentPath() + "/control-dashboard-XXXXXX.societycontainer");
        CTEST_CHECK(SocietyDrive::create(root.path()));
        const auto control = root.filePath("Files/.iisacc-file-provider");
        CTEST_CHECK(QDir().mkpath(control));
        for (const auto &name : {"catalog.sqlite3", "catalog.sqlite3-wal", "catalog.sqlite3-shm"}) {
            QFile file(control + '/' + name); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("internal");
        }
        QFile visible(root.filePath("Files/document.txt"));
        CTEST_CHECK(visible.open(QIODevice::WriteOnly)); visible.write("visible"); visible.close();
        DashboardFiles model; model.setContainerPath(root.path());
        CTEST_WAIT_DEFAULT(!model.loading());
        CTEST_COMPARE(model.recentFiles().size(), 1);
        CTEST_COMPARE(model.recentFiles().first().toMap()["name"].toString(), "document.txt");
    }
    void editorCanvasesDoNotDuplicateGeneratedImagesInHistory() {
        QTemporaryDir root(QDir::currentPath() + "/native-history-XXXXXX.societycontainer");
        CTEST_CHECK(SocietyDrive::create(root.path()));
        QFile canvas(root.filePath("Generation History/generated.iisc"));
        CTEST_CHECK(canvas.open(QIODevice::WriteOnly)); canvas.write("native document"); canvas.close();
        QImage png(16, 16, QImage::Format_RGB32); png.fill(Qt::cyan);
        CTEST_CHECK(png.save(root.filePath("Generation History/generated.png")));
        DashboardFiles model; model.setContainerPath(root.path());
        CTEST_WAIT_DEFAULT(!model.loading());
        CTEST_COMPARE(model.generationHistory().size(), 1);
        const auto row = model.generationHistory().first().toMap();
        CTEST_COMPARE(row.value("name").toString(), "generated.png");
        CTEST_COMPARE(row.value("previewSource").toUrl().toLocalFile(), root.filePath("Generation History/generated.png"));
        CTEST_CHECK(row.value("metadata1").toString().startsWith("PNG"));
    }
    void concurrentSectionsKeepMetadataIndependent() {
        QTemporaryDir root(QDir::currentPath() + "/parallel-dashboard-XXXXXX.societycontainer");
        CTEST_CHECK(SocietyDrive::create(root.path()));
        QImage image(16, 16, QImage::Format_RGB32); image.fill(Qt::cyan);
        for (const auto &section : {"Files", "Published", "Generation History"})
            for (int i = 0; i < 8; ++i)
                CTEST_CHECK(image.save(root.filePath(QString("%1/item-%2.png").arg(section).arg(i))));
        DashboardFiles model;
        model.setContainerPath(root.path());
        for (int iteration = 0; iteration < 12; ++iteration) {
            CTEST_WAIT_VERIFY(!model.loading(), 30000);
            CTEST_CHECK(model.errorString().isEmpty());
            const auto verify = [&](const QVariantList &rows, const QString &section, int count) {
                CTEST_COMPARE(rows.size(), count);
                for (const auto &entry : rows) {
                    const auto row = entry.toMap();
                    CTEST_CHECK(row.value("path").toString().contains('/' + section + '/'));
                    const QFileInfo file(row.value("path").toString());
                    CTEST_COMPARE(row.value("metadata1").toString(), "PNG · " + QLocale().formattedDataSize(file.size()));
                    CTEST_CHECK(!row.contains("_scanByteSize"));
                }
            };
            verify(model.recentFiles(), "Files", 8);
            verify(model.recentPublished(), "Published", 4);
            verify(model.generationHistory(), "Generation History", 8);
            if (iteration != 11) model.refresh();
        }
    }
    void historyIsPersistedBoundedAndObserved() {
        QTemporaryDir root(QDir::currentPath() + "/history-XXXXXX.societycontainer");
        CTEST_CHECK(SocietyDrive::create(root.path()));
        QImage image(16, 16, QImage::Format_RGB32); image.fill(Qt::cyan);
        for (int i = 0; i < 25; ++i) {
            const auto path = root.filePath(QString("Generation History/image-%1.png").arg(i, 2, 10, QChar('0')));
            CTEST_CHECK(image.save(path));
            QFile file(path); CTEST_CHECK(file.open(QIODevice::ReadWrite));
            CTEST_CHECK(file.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-60-i), QFileDevice::FileModificationTime));
        }
        CTEST_CHECK(image.save(root.filePath("Files/separate.png")));
        for (int i = 0; i < 6; ++i) {
            const auto path = root.filePath(QString("Published/published-%1.png").arg(i, 2, 10, QChar('0')));
            CTEST_CHECK(image.save(path));
            QFile file(path); CTEST_CHECK(file.open(QIODevice::ReadWrite));
            CTEST_CHECK(file.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-30-i), QFileDevice::FileModificationTime));
        }
        CTEST_CHECK(image.save(root.filePath("Generation History/.hidden.png")));
        CTEST_CHECK(QDir().mkpath(root.filePath("Generation History/legacy")));
        CTEST_CHECK(image.save(root.filePath("Generation History/legacy/nested.png")));
        CTEST_CHECK(createTestSymbolicLink(root.filePath("Files/separate.png"), root.filePath("Generation History/link.png")));
        QFile metadata(root.filePath("Generation History/request.json"));
        CTEST_CHECK(metadata.open(QIODevice::WriteOnly)); metadata.write("{}"); metadata.close();
        DashboardFiles model; model.setContainerPath(root.path());
        CTEST_WAIT_DEFAULT(!model.loading());
        CTEST_COMPARE(model.generationHistory().size(), 20);
        CTEST_COMPARE(model.generationHistory().first().toMap().value("name").toString(), "image-00.png");
        CTEST_COMPARE(model.generationHistory().last().toMap().value("name").toString(), "image-19.png");
        CTEST_COMPARE(model.recentFiles().size(), 1);
        CTEST_COMPARE(model.recentFiles().first().toMap().value("previewSource").toUrl().scheme(), QString("file"));
        CTEST_COMPARE(model.recentFiles().first().toMap().value("thumbnailSource").toUrl().host(), QString("society-preview"));
        CTEST_COMPARE(model.recentPublished().size(), 4);
        CTEST_COMPARE(model.recentPublished().first().toMap().value("name").toString(), "published-00.png");
        CTEST_COMPARE(model.recentPublished().last().toMap().value("name").toString(), "published-03.png");
        society_ctest::SignalRecorder changed(&model, &DashboardFiles::filesChanged);
        model.refresh(); CTEST_WAIT_DEFAULT(!model.loading());
        CTEST_COMPARE(changed.size(), 0);
        CTEST_CHECK(image.save(root.filePath("Generation History/latest.png")));
        CTEST_WAIT_COMPARE(model.generationHistory().first().toMap().value("name").toString(), "latest.png");
        const auto preview = model.generationHistory().first().toMap().value("previewSource");
        QSaveFile replacement(root.filePath("Generation History/latest.png"));
        CTEST_CHECK(replacement.open(QIODevice::WriteOnly));
        QImage larger(64, 64, QImage::Format_RGB32); larger.fill(Qt::yellow);
        CTEST_CHECK(larger.save(&replacement, "PNG")); CTEST_CHECK(replacement.commit());
        CTEST_WAIT_DEFAULT(model.generationHistory().first().toMap().value("previewSource") != preview);
        CTEST_CHECK(QFile::remove(root.filePath("Generation History/latest.png")));
        CTEST_WAIT_COMPARE(model.generationHistory().first().toMap().value("name").toString(), "image-00.png");
        model.setQuery("image-24");
        CTEST_COMPARE(model.generationHistory().size(), 1);
        CTEST_COMPARE(model.generationHistory().first().toMap().value("name").toString(), "image-24.png");
        model.refresh(); model.setContainerPath("");
        CTEST_WAIT_DEFAULT(!model.loading()); CTEST_CHECK(model.generationHistory().isEmpty());
        model.setContainerPath("relative");
        CTEST_CHECK(model.generationHistory().isEmpty()); CTEST_CHECK(!model.errorString().isEmpty());
    }
    void onlyHistoryLinksAreAccepted() {
        SocietyApplication application;
        society_ctest::SignalRecorder requested(&application, &SocietyApplication::generationHistoryRequested);
        for (const auto &url : {"https://generation-history", "society://models", "society://generation-history/../Models",
                               "society://generation-history?path=/tmp", "society://user@generation-history", "society://generation-history:123"})
            CTEST_CHECK(!application.handleUrl(QUrl(url)));
        CTEST_COMPARE(requested.size(), 0);
        CTEST_CHECK(application.handleUrl(QUrl("society://generation-history")));
        CTEST_COMPARE(requested.size(), 1);
        application.listen();
        QFileOpenEvent event(QUrl("society://generation-history"));
        QCoreApplication::sendEvent(QCoreApplication::instance(), &event);
        CTEST_COMPARE(requested.size(), 2);
        CTEST_CHECK(application.openGenerationHistory());
        CTEST_COMPARE(requested.size(), 3);
    }
};
int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    society_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    DashboardTests tests;
    runner.run("providerMetadataAndOntologyAreTheActualSearchSource", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.providerMetadataAndOntologyAreTheActualSearchSource(); });
    runner.run("documentsExposeRevisionedPreviewsWithoutBecomingImages", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.documentsExposeRevisionedPreviewsWithoutBecomingImages(); });
    runner.run("providerControlDirectoryContentsNeverBecomeRecentFiles", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.providerControlDirectoryContentsNeverBecomeRecentFiles(); });
    runner.run("editorCanvasesDoNotDuplicateGeneratedImagesInHistory", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.editorCanvasesDoNotDuplicateGeneratedImagesInHistory(); });
    runner.run("concurrentSectionsKeepMetadataIndependent", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.concurrentSectionsKeepMetadataIndependent(); });
    runner.run("historyIsPersistedBoundedAndObserved", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.historyIsPersistedBoundedAndObserved(); });
    runner.run("onlyHistoryLinksAreAccepted", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.onlyHistoryLinksAreAccepted(); });
    return runner.result();
}
