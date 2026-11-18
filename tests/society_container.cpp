#include "SymbolicLink.h"
#include <src/Store/StoreSection.h>
#include <iiSocietyContainer.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/qscopeguard.h>
#include "CTestSupport.h"
#include <QCoreApplication>
#include <QEvent>

#include <memory>

using iiSocietyContainer::SocietyContainer;
using iiSocietyContainer::StoreSection;
using iiSocietyContainer::allStoreSections;
using iiSocietyContainer::storeSectionName;
using PathKind = SocietyContainer::PathKind;

class SocietyContainerTests : public QObject
{
public:
    void accessesMetadataAndNestedTree()
    {
        const SocietyContainer container(root);
        const auto snapshot = container.tree();
        CTEST_CHECK(snapshot);
        const auto* note = snapshot.value->find("assets/note.txt");
        CTEST_CHECK(note);
        CTEST_COMPARE(note->size, std::uintmax_t(16));
        CTEST_CHECK(note->parentPath == "assets");
        const auto children = container.entries("assets");
        CTEST_CHECK(children); CTEST_COMPARE(children.value->size(), std::size_t(1));
        CTEST_CHECK(container.entry("assets/note.txt"));
        CTEST_CHECK(!container.entry("../Space-other/child"));
        CTEST_CHECK(!container.tree("assets/note.txt/child"));
        CTEST_CHECK(QDir(root).removeRecursively());
        CTEST_CHECK(!container.tree());
    }

    void init()
    {
        workspace = std::make_unique<QTemporaryDir>(
            QDir::current().filePath(QStringLiteral("society-container-XXXXXX")));
        CTEST_CHECK(workspace->isValid());
        root = workspace->filePath(QStringLiteral("Space"));
        sibling = workspace->filePath(QStringLiteral("Space-other"));
        CTEST_CHECK(QDir().mkpath(root + QStringLiteral("/assets")));
        CTEST_CHECK(QDir().mkpath(sibling + QStringLiteral("/child")));
        QFile file(root + QStringLiteral("/assets/note.txt"));
        CTEST_CHECK(file.open(QIODevice::WriteOnly));
        CTEST_COMPARE(file.write("existing content"), qint64(16));
    }

    void acceptsDirectoryWithoutChangingContents()
    {
        const auto filters = QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
        const auto entries = QDir(root).entryList(filters);
        const SocietyContainer container(root);

        CTEST_CHECK(container.isValid());
        CTEST_CHECK(container.errorString().isEmpty());
        CTEST_COMPARE(container.rootPath(), QFileInfo(root).canonicalFilePath());
        CTEST_COMPARE(container.classifyPath(root), PathKind::Root);
        CTEST_COMPARE(container.classifyPath(QStringLiteral(".")), PathKind::Root);
        CTEST_COMPARE(container.classifyPath(root + QStringLiteral("/assets")), PathKind::Entry);
        CTEST_COMPARE(container.classifyPath(root + QStringLiteral("/assets/note.txt")), PathKind::Entry);
        CTEST_COMPARE(QDir(root).entryList(filters), entries);

        QFile file(root + QStringLiteral("/assets/note.txt"));
        CTEST_CHECK(file.open(QIODevice::ReadOnly));
        CTEST_COMPARE(file.readAll(), QByteArray("existing content"));
    }

    void keepsSpacesIndependent()
    {
        const SocietyContainer first(root);
        const SocietyContainer second(sibling);

        CTEST_COMPARE(first.classifyPath(sibling), PathKind::Outside);
        CTEST_COMPARE(second.classifyPath(root), PathKind::Outside);
        CTEST_COMPARE(first.classifyPath(workspace->path()), PathKind::Outside);
        CTEST_COMPARE(second.classifyPath(sibling), PathKind::Root);
        CTEST_COMPARE(first.classifyPath(sibling + QStringLiteral("/child")), PathKind::Outside);
    }

    void normalizesAndPinsRelativeRoot()
    {
        const QString originalDirectory = QDir::currentPath();
        const auto restoreDirectory = qScopeGuard([originalDirectory] {
            QDir::setCurrent(originalDirectory);
        });
        const QString relativeRoot = QDir::current().relativeFilePath(root);
        const SocietyContainer container(relativeRoot + QStringLiteral("/assets/.././"));

        CTEST_CHECK(container.isValid());
        CTEST_COMPARE(container.rootPath(), QFileInfo(root).canonicalFilePath());
        CTEST_CHECK(QDir::setCurrent(sibling));
        CTEST_CHECK(container.isValid());
        CTEST_COMPARE(container.classifyPath(QStringLiteral(".")), PathKind::Root);
        CTEST_COMPARE(container.classifyPath(QStringLiteral("assets/note.txt")), PathKind::Entry);
    }

    void acceptsUnicodeAndWhitespaceInDirectoryNames()
    {
        const QString path = workspace->filePath(QStringLiteral(" 자료 Space "));
        CTEST_CHECK(QDir().mkdir(path));
        const SocietyContainer container(path);
        CTEST_CHECK(container.isValid());
        CTEST_COMPARE(container.classifyPath(path), PathKind::Root);
        CTEST_COMPARE(container.rootPath(), QFileInfo(path).canonicalFilePath());
    }

    void rejectsInvalidRoots(QString input)
    {
        input.replace(QStringLiteral("{root}"), root);
        const SocietyContainer container(input);

        CTEST_CHECK(!container.isValid());
        CTEST_CHECK(container.rootPath().isEmpty());
        CTEST_CHECK(!container.errorString().isEmpty());
        CTEST_COMPARE(container.classifyPath(root), PathKind::Outside);
        CTEST_CHECK(container.sections().isEmpty());
        for (const auto section : allStoreSections()) {
            CTEST_CHECK(!container.hasSection(section));
        }
        CTEST_CHECK(!QFileInfo::exists(root + QStringLiteral("/missing")));
    }

    void classifiesRelativePaths(const QString& input, PathKind expected)
    {
        CTEST_COMPARE(SocietyContainer(root).classifyPath(input), expected);
    }

    void resolvesSymbolicLinksBeforeClassification()
    {
#ifdef Q_OS_UNIX
        const QString rootAlias = workspace->filePath(QStringLiteral("root-alias"));
        const QString inwardAlias = workspace->filePath(QStringLiteral("inward-alias"));
        const QString outwardAlias = root + QStringLiteral("/outward-alias");
        const QString brokenAlias = root + QStringLiteral("/broken-alias");
        CTEST_CHECK(createTestSymbolicLink(root, rootAlias));
        CTEST_CHECK(createTestSymbolicLink(root + QStringLiteral("/assets/note.txt"), inwardAlias));
        CTEST_CHECK(createTestSymbolicLink(sibling + QStringLiteral("/child"), outwardAlias));
        CTEST_CHECK(createTestSymbolicLink(root + QStringLiteral("/missing"), brokenAlias));

        const SocietyContainer container(rootAlias);
        CTEST_CHECK(container.isValid());
        CTEST_COMPARE(container.rootPath(), QFileInfo(root).canonicalFilePath());
        CTEST_COMPARE(container.classifyPath(rootAlias), PathKind::Root);
        CTEST_COMPARE(container.classifyPath(inwardAlias), PathKind::Entry);
        CTEST_COMPARE(container.classifyPath(outwardAlias), PathKind::Outside);
        CTEST_COMPARE(container.classifyPath(outwardAlias + QStringLiteral("/..")), PathKind::Outside);
        CTEST_COMPARE(container.classifyPath(brokenAlias), PathKind::Outside);
        CTEST_CHECK(!SocietyContainer(brokenAlias).isValid());

        CTEST_CHECK(QFile::remove(rootAlias));
        CTEST_CHECK(createTestSymbolicLink(sibling, rootAlias));
        CTEST_CHECK(container.isValid());
        CTEST_COMPARE(container.classifyPath(rootAlias), PathKind::Outside);
        CTEST_COMPARE(container.classifyPath(root), PathKind::Root);
#else
        society_ctest::skip("QFile::link creates shortcuts instead of symbolic links on Windows.");
#endif
    }

    void detectsRemovedOrRedirectedRoot()
    {
        const SocietyContainer container(root);
        CTEST_CHECK(QDir(root).removeRecursively());
        CTEST_CHECK(!container.isValid());
        CTEST_CHECK(!container.errorString().isEmpty());
        CTEST_COMPARE(container.classifyPath(root), PathKind::Outside);
        CTEST_COMPARE(container.classifyPath(QStringLiteral(".")), PathKind::Outside);
        CTEST_CHECK(container.sections().isEmpty());
        CTEST_CHECK(!container.hasSection(StoreSection::Files));

#ifdef Q_OS_UNIX
        CTEST_CHECK(createTestSymbolicLink(sibling, root));
        CTEST_CHECK(!container.isValid());
        CTEST_CHECK(!container.errorString().isEmpty());
        CTEST_COMPARE(container.classifyPath(root), PathKind::Outside);
        CTEST_CHECK(container.sections().isEmpty());
        CTEST_CHECK(!container.hasSection(StoreSection::Files));
#endif
    }

    void exposesExactlyTheNineLogicalSections()
    {
        const QList<StoreSection> expectedSections{
            StoreSection::AssetLibrary,
            StoreSection::Deleted,
            StoreSection::Files,
            StoreSection::Forked,
            StoreSection::GenerationHistory,
            StoreSection::Models,
            StoreSection::Photos,
            StoreSection::Published,
            StoreSection::ThinkingSpace
        };
        const QStringList expectedNames{
            QStringLiteral("Asset Library"),
            QStringLiteral("Deleted"),
            QStringLiteral("Files"),
            QStringLiteral("Forked"),
            QStringLiteral("Generation History"),
            QStringLiteral("Models"),
            QStringLiteral("Photos"),
            QStringLiteral("Published"),
            QStringLiteral("Thinking Space")
        };
        const SocietyContainer container(root);

        CTEST_COMPARE(allStoreSections(), expectedSections);
        CTEST_COMPARE(container.sections(), expectedSections);
        QStringList names;
        for (const auto section : container.sections()) {
            CTEST_CHECK(container.hasSection(section));
            names.append(storeSectionName(section));
        }
        CTEST_COMPARE(names, expectedNames);
    }

    void logicalSectionsAreIndependentOfPhysicalLayout()
    {
        const QString emptyRoot = workspace->filePath(QStringLiteral("Empty Space"));
        CTEST_CHECK(QDir().mkdir(emptyRoot));
        const SocietyContainer emptyContainer(emptyRoot);
        const SocietyContainer populatedContainer(root);
        const auto filters = QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
        const auto originalEntries = QDir(root).entryList(filters);

        CTEST_COMPARE(emptyContainer.sections(), allStoreSections());
        CTEST_COMPARE(populatedContainer.sections(), allStoreSections());
        for (const auto section : allStoreSections()) {
            CTEST_CHECK(emptyContainer.hasSection(section));
            CTEST_CHECK(populatedContainer.hasSection(section));
        }
        CTEST_CHECK(QDir(emptyRoot).entryList(filters).isEmpty());
        CTEST_COMPARE(QDir(root).entryList(filters), originalEntries);

        // Matching display names do not impose directory types or storage rules.
        QFile namedFile(root + QStringLiteral("/Deleted"));
        CTEST_CHECK(namedFile.open(QIODevice::WriteOnly));
        namedFile.close();
        CTEST_CHECK(QDir().mkdir(root + QStringLiteral("/Models")));
        const SocietyContainer reopened(root);
        CTEST_CHECK(reopened.isValid());
        CTEST_COMPARE(reopened.sections(), allStoreSections());
        CTEST_COMPARE(reopened.classifyPath(QStringLiteral("Deleted")), PathKind::Entry);
        CTEST_COMPARE(reopened.classifyPath(QStringLiteral("Models")), PathKind::Entry);

        CTEST_CHECK(QDir(emptyRoot).removeRecursively());
        CTEST_CHECK(emptyContainer.sections().isEmpty());
        CTEST_CHECK(!emptyContainer.hasSection(StoreSection::Files));
        CTEST_COMPARE(populatedContainer.sections(), allStoreSections());
    }

    void rejectsUnknownSectionIdentifiers()
    {
        const SocietyContainer container(root);
        for (const int value : {-1, 9, 1000}) {
            const auto unknown = static_cast<StoreSection>(value);
            CTEST_CHECK(storeSectionName(unknown).isEmpty());
            CTEST_CHECK(!container.hasSection(unknown));
            CTEST_CHECK(!container.sections().contains(unknown));
        }
    }

    void supportsFilesystemRoot()
    {
        QDir volumeRoot(root);
        while (!volumeRoot.isRoot()) CTEST_CHECK(volumeRoot.cdUp());
        const SocietyContainer container(volumeRoot.absolutePath());
        CTEST_CHECK(container.isValid());
        CTEST_COMPARE(container.classifyPath(container.rootPath()), PathKind::Root);
        CTEST_COMPARE(container.classifyPath(root), PathKind::Entry);
    }

private:
    std::unique_ptr<QTemporaryDir> workspace;
    QString root;
    QString sibling;
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name" << std::endl; return 1; }
    const std::string_view selection = argc == 2 ? argv[1] : "";
    const bool roots = selection == "rejectsInvalidRoots";
    const bool paths = selection == "classifiesRelativePaths";
    society_ctest::CTestRunner runner(roots || paths ? std::string_view{} : selection);
    const auto invoke = [](auto&& test) {
        const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); });
        SocietyContainerTests tests;
        tests.init();
        test(tests);
    };
    if (!roots && !paths) runner.run("accessesMetadataAndNestedTree", [&] { invoke([](auto& tests) { tests.accessesMetadataAndNestedTree(); }); });
    if (!roots && !paths) runner.run("acceptsDirectoryWithoutChangingContents", [&] { invoke([](auto& tests) { tests.acceptsDirectoryWithoutChangingContents(); }); });
    if (!roots && !paths) runner.run("keepsSpacesIndependent", [&] { invoke([](auto& tests) { tests.keepsSpacesIndependent(); }); });
    if (!roots && !paths) runner.run("normalizesAndPinsRelativeRoot", [&] { invoke([](auto& tests) { tests.normalizesAndPinsRelativeRoot(); }); });
    if (!roots && !paths) runner.run("acceptsUnicodeAndWhitespaceInDirectoryNames", [&] { invoke([](auto& tests) { tests.acceptsUnicodeAndWhitespaceInDirectoryNames(); }); });
    if (!paths) runner.run("rejectsInvalidRoots:empty", [&] { invoke([](auto& tests) { tests.rejectsInvalidRoots(QString()); }); });
    if (!paths) runner.run("rejectsInvalidRoots:missing", [&] { invoke([](auto& tests) { tests.rejectsInvalidRoots(QStringLiteral("{root}/missing")); }); });
    if (!paths) runner.run("rejectsInvalidRoots:file", [&] { invoke([](auto& tests) { tests.rejectsInvalidRoots(QStringLiteral("{root}/assets/note.txt")); }); });
    if (!paths) runner.run("rejectsInvalidRoots:file-as-parent", [&] { invoke([](auto& tests) { tests.rejectsInvalidRoots(QStringLiteral("{root}/assets/note.txt/child")); }); });
    if (!paths) runner.run("rejectsInvalidRoots:embedded-null", [&] { invoke([](auto& tests) { tests.rejectsInvalidRoots(QStringLiteral("{root}") + QChar::Null + "suffix"); }); });
    if (!paths) runner.run("rejectsInvalidRoots:qt-resource", [&] { invoke([](auto& tests) { tests.rejectsInvalidRoots(QStringLiteral(":/")); }); });
    if (!roots) runner.run("classifiesRelativePaths:root", [&] { invoke([](auto& tests) { tests.classifiesRelativePaths(QStringLiteral("."), PathKind::Root); }); });
    if (!roots) runner.run("classifiesRelativePaths:normalized-root", [&] { invoke([](auto& tests) { tests.classifiesRelativePaths(QStringLiteral("./assets/../"), PathKind::Root); }); });
    if (!roots) runner.run("classifiesRelativePaths:directory", [&] { invoke([](auto& tests) { tests.classifiesRelativePaths(QStringLiteral("assets"), PathKind::Entry); }); });
    if (!roots) runner.run("classifiesRelativePaths:file", [&] { invoke([](auto& tests) { tests.classifiesRelativePaths(QStringLiteral("assets/note.txt"), PathKind::Entry); }); });
    if (!roots) runner.run("classifiesRelativePaths:parent", [&] { invoke([](auto& tests) { tests.classifiesRelativePaths(QStringLiteral(".."), PathKind::Outside); }); });
    if (!roots) runner.run("classifiesRelativePaths:same-prefix-sibling", [&] { invoke([](auto& tests) { tests.classifiesRelativePaths(QStringLiteral("../Space-other"), PathKind::Outside); }); });
    if (!roots) runner.run("classifiesRelativePaths:traversal", [&] { invoke([](auto& tests) { tests.classifiesRelativePaths(QStringLiteral("assets/../../Space-other/child"), PathKind::Outside); }); });
    if (!roots) runner.run("classifiesRelativePaths:missing", [&] { invoke([](auto& tests) { tests.classifiesRelativePaths(QStringLiteral("missing.txt"), PathKind::Outside); }); });
    if (!roots) runner.run("classifiesRelativePaths:empty", [&] { invoke([](auto& tests) { tests.classifiesRelativePaths(QString(), PathKind::Outside); }); });
    if (!roots) runner.run("classifiesRelativePaths:embedded-null", [&] { invoke([](auto& tests) { tests.classifiesRelativePaths(QStringLiteral("assets") + QChar::Null + "suffix", PathKind::Outside); }); });
    if (!roots) runner.run("classifiesRelativePaths:qt-resource", [&] { invoke([](auto& tests) { tests.classifiesRelativePaths(QStringLiteral(":/"), PathKind::Outside); }); });
    if (!roots && !paths) runner.run("resolvesSymbolicLinksBeforeClassification", [&] { invoke([](auto& tests) { tests.resolvesSymbolicLinksBeforeClassification(); }); });
    if (!roots && !paths) runner.run("detectsRemovedOrRedirectedRoot", [&] { invoke([](auto& tests) { tests.detectsRemovedOrRedirectedRoot(); }); });
    if (!roots && !paths) runner.run("exposesExactlyTheNineLogicalSections", [&] { invoke([](auto& tests) { tests.exposesExactlyTheNineLogicalSections(); }); });
    if (!roots && !paths) runner.run("logicalSectionsAreIndependentOfPhysicalLayout", [&] { invoke([](auto& tests) { tests.logicalSectionsAreIndependentOfPhysicalLayout(); }); });
    if (!roots && !paths) runner.run("rejectsUnknownSectionIdentifiers", [&] { invoke([](auto& tests) { tests.rejectsUnknownSectionIdentifiers(); }); });
    if (!roots && !paths) runner.run("supportsFilesystemRoot", [&] { invoke([](auto& tests) { tests.supportsFilesystemRoot(); }); });
    return runner.result();
}
