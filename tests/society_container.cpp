#include <src/Store/StoreSection.h>
#include <iiSocietyContainer.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/qscopeguard.h>
#include <QtTest/QTest>

#include <memory>

using iiSocietyContainer::SocietyContainer;
using iiSocietyContainer::StoreSection;
using iiSocietyContainer::allStoreSections;
using iiSocietyContainer::storeSectionName;
using PathKind = SocietyContainer::PathKind;

class SocietyContainerTests : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        workspace = std::make_unique<QTemporaryDir>(
            QDir::current().filePath(QStringLiteral("society-container-XXXXXX")));
        QVERIFY(workspace->isValid());
        root = workspace->filePath(QStringLiteral("Space"));
        sibling = workspace->filePath(QStringLiteral("Space-other"));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/assets")));
        QVERIFY(QDir().mkpath(sibling + QStringLiteral("/child")));
        QFile file(root + QStringLiteral("/assets/note.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("existing content"), qint64(16));
    }

    void acceptsDirectoryWithoutChangingContents()
    {
        const auto filters = QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
        const auto entries = QDir(root).entryList(filters);
        const SocietyContainer container(root);

        QVERIFY(container.isValid());
        QVERIFY(container.errorString().isEmpty());
        QCOMPARE(container.rootPath(), QFileInfo(root).canonicalFilePath());
        QCOMPARE(container.classifyPath(root), PathKind::Root);
        QCOMPARE(container.classifyPath(QStringLiteral(".")), PathKind::Root);
        QCOMPARE(container.classifyPath(root + QStringLiteral("/assets")), PathKind::Entry);
        QCOMPARE(container.classifyPath(root + QStringLiteral("/assets/note.txt")), PathKind::Entry);
        QCOMPARE(QDir(root).entryList(filters), entries);

        QFile file(root + QStringLiteral("/assets/note.txt"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("existing content"));
    }

    void keepsSpacesIndependent()
    {
        const SocietyContainer first(root);
        const SocietyContainer second(sibling);

        QCOMPARE(first.classifyPath(sibling), PathKind::Outside);
        QCOMPARE(second.classifyPath(root), PathKind::Outside);
        QCOMPARE(first.classifyPath(workspace->path()), PathKind::Outside);
        QCOMPARE(second.classifyPath(sibling), PathKind::Root);
        QCOMPARE(first.classifyPath(sibling + QStringLiteral("/child")), PathKind::Outside);
    }

    void normalizesAndPinsRelativeRoot()
    {
        const QString originalDirectory = QDir::currentPath();
        const auto restoreDirectory = qScopeGuard([originalDirectory] {
            QDir::setCurrent(originalDirectory);
        });
        const QString relativeRoot = QDir::current().relativeFilePath(root);
        const SocietyContainer container(relativeRoot + QStringLiteral("/assets/.././"));

        QVERIFY(container.isValid());
        QCOMPARE(container.rootPath(), QFileInfo(root).canonicalFilePath());
        QVERIFY(QDir::setCurrent(sibling));
        QVERIFY(container.isValid());
        QCOMPARE(container.classifyPath(QStringLiteral(".")), PathKind::Root);
        QCOMPARE(container.classifyPath(QStringLiteral("assets/note.txt")), PathKind::Entry);
    }

    void acceptsUnicodeAndWhitespaceInDirectoryNames()
    {
        const QString path = workspace->filePath(QStringLiteral(" 자료 Space "));
        QVERIFY(QDir().mkdir(path));
        const SocietyContainer container(path);
        QVERIFY(container.isValid());
        QCOMPARE(container.classifyPath(path), PathKind::Root);
        QCOMPARE(container.rootPath(), QFileInfo(path).canonicalFilePath());
    }

    void rejectsInvalidRoots_data()
    {
        QTest::addColumn<QString>("input");
        QTest::newRow("empty") << QString();
        QTest::newRow("missing") << QStringLiteral("{root}/missing");
        QTest::newRow("file") << QStringLiteral("{root}/assets/note.txt");
        QTest::newRow("file-as-parent") << QStringLiteral("{root}/assets/note.txt/child");
        QTest::newRow("embedded-null") << (QStringLiteral("{root}") + QChar::Null + "suffix");
        QTest::newRow("qt-resource") << QStringLiteral(":/");
    }

    void rejectsInvalidRoots()
    {
        QFETCH(QString, input);
        input.replace(QStringLiteral("{root}"), root);
        const SocietyContainer container(input);

        QVERIFY(!container.isValid());
        QVERIFY(container.rootPath().isEmpty());
        QVERIFY(!container.errorString().isEmpty());
        QCOMPARE(container.classifyPath(root), PathKind::Outside);
        QVERIFY(container.sections().isEmpty());
        for (const auto section : allStoreSections()) {
            QVERIFY(!container.hasSection(section));
        }
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/missing")));
    }

    void classifiesRelativePaths_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<PathKind>("expected");
        QTest::newRow("root") << QStringLiteral(".") << PathKind::Root;
        QTest::newRow("normalized-root") << QStringLiteral("./assets/../") << PathKind::Root;
        QTest::newRow("directory") << QStringLiteral("assets") << PathKind::Entry;
        QTest::newRow("file") << QStringLiteral("assets/note.txt") << PathKind::Entry;
        QTest::newRow("parent") << QStringLiteral("..") << PathKind::Outside;
        QTest::newRow("same-prefix-sibling") << QStringLiteral("../Space-other") << PathKind::Outside;
        QTest::newRow("traversal") << QStringLiteral("assets/../../Space-other/child") << PathKind::Outside;
        QTest::newRow("missing") << QStringLiteral("missing.txt") << PathKind::Outside;
        QTest::newRow("empty") << QString() << PathKind::Outside;
        QTest::newRow("embedded-null") << (QStringLiteral("assets") + QChar::Null + "suffix") << PathKind::Outside;
        QTest::newRow("qt-resource") << QStringLiteral(":/") << PathKind::Outside;
    }

    void classifiesRelativePaths()
    {
        QFETCH(QString, input);
        QFETCH(PathKind, expected);
        QCOMPARE(SocietyContainer(root).classifyPath(input), expected);
    }

    void resolvesSymbolicLinksBeforeClassification()
    {
#ifdef Q_OS_UNIX
        const QString rootAlias = workspace->filePath(QStringLiteral("root-alias"));
        const QString inwardAlias = workspace->filePath(QStringLiteral("inward-alias"));
        const QString outwardAlias = root + QStringLiteral("/outward-alias");
        const QString brokenAlias = root + QStringLiteral("/broken-alias");
        QVERIFY(QFile::link(root, rootAlias));
        QVERIFY(QFile::link(root + QStringLiteral("/assets/note.txt"), inwardAlias));
        QVERIFY(QFile::link(sibling + QStringLiteral("/child"), outwardAlias));
        QVERIFY(QFile::link(root + QStringLiteral("/missing"), brokenAlias));

        const SocietyContainer container(rootAlias);
        QVERIFY(container.isValid());
        QCOMPARE(container.rootPath(), QFileInfo(root).canonicalFilePath());
        QCOMPARE(container.classifyPath(rootAlias), PathKind::Root);
        QCOMPARE(container.classifyPath(inwardAlias), PathKind::Entry);
        QCOMPARE(container.classifyPath(outwardAlias), PathKind::Outside);
        QCOMPARE(container.classifyPath(outwardAlias + QStringLiteral("/..")), PathKind::Outside);
        QCOMPARE(container.classifyPath(brokenAlias), PathKind::Outside);
        QVERIFY(!SocietyContainer(brokenAlias).isValid());

        QVERIFY(QFile::remove(rootAlias));
        QVERIFY(QFile::link(sibling, rootAlias));
        QVERIFY(container.isValid());
        QCOMPARE(container.classifyPath(rootAlias), PathKind::Outside);
        QCOMPARE(container.classifyPath(root), PathKind::Root);
#else
        QSKIP("QFile::link creates shortcuts instead of symbolic links on Windows.");
#endif
    }

    void detectsRemovedOrRedirectedRoot()
    {
        const SocietyContainer container(root);
        QVERIFY(QDir(root).removeRecursively());
        QVERIFY(!container.isValid());
        QVERIFY(!container.errorString().isEmpty());
        QCOMPARE(container.classifyPath(root), PathKind::Outside);
        QCOMPARE(container.classifyPath(QStringLiteral(".")), PathKind::Outside);
        QVERIFY(container.sections().isEmpty());
        QVERIFY(!container.hasSection(StoreSection::Files));

#ifdef Q_OS_UNIX
        QVERIFY(QFile::link(sibling, root));
        QVERIFY(!container.isValid());
        QVERIFY(!container.errorString().isEmpty());
        QCOMPARE(container.classifyPath(root), PathKind::Outside);
        QVERIFY(container.sections().isEmpty());
        QVERIFY(!container.hasSection(StoreSection::Files));
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

        QCOMPARE(allStoreSections(), expectedSections);
        QCOMPARE(container.sections(), expectedSections);
        QStringList names;
        for (const auto section : container.sections()) {
            QVERIFY(container.hasSection(section));
            names.append(storeSectionName(section));
        }
        QCOMPARE(names, expectedNames);
    }

    void logicalSectionsAreIndependentOfPhysicalLayout()
    {
        const QString emptyRoot = workspace->filePath(QStringLiteral("Empty Space"));
        QVERIFY(QDir().mkdir(emptyRoot));
        const SocietyContainer emptyContainer(emptyRoot);
        const SocietyContainer populatedContainer(root);
        const auto filters = QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
        const auto originalEntries = QDir(root).entryList(filters);

        QCOMPARE(emptyContainer.sections(), allStoreSections());
        QCOMPARE(populatedContainer.sections(), allStoreSections());
        for (const auto section : allStoreSections()) {
            QVERIFY(emptyContainer.hasSection(section));
            QVERIFY(populatedContainer.hasSection(section));
        }
        QVERIFY(QDir(emptyRoot).entryList(filters).isEmpty());
        QCOMPARE(QDir(root).entryList(filters), originalEntries);

        // Matching display names do not impose directory types or storage rules.
        QFile namedFile(root + QStringLiteral("/Deleted"));
        QVERIFY(namedFile.open(QIODevice::WriteOnly));
        namedFile.close();
        QVERIFY(QDir().mkdir(root + QStringLiteral("/Models")));
        const SocietyContainer reopened(root);
        QVERIFY(reopened.isValid());
        QCOMPARE(reopened.sections(), allStoreSections());
        QCOMPARE(reopened.classifyPath(QStringLiteral("Deleted")), PathKind::Entry);
        QCOMPARE(reopened.classifyPath(QStringLiteral("Models")), PathKind::Entry);

        QVERIFY(QDir(emptyRoot).removeRecursively());
        QVERIFY(emptyContainer.sections().isEmpty());
        QVERIFY(!emptyContainer.hasSection(StoreSection::Files));
        QCOMPARE(populatedContainer.sections(), allStoreSections());
    }

    void rejectsUnknownSectionIdentifiers()
    {
        const SocietyContainer container(root);
        for (const int value : {-1, 9, 1000}) {
            const auto unknown = static_cast<StoreSection>(value);
            QVERIFY(storeSectionName(unknown).isEmpty());
            QVERIFY(!container.hasSection(unknown));
            QVERIFY(!container.sections().contains(unknown));
        }
    }

    void supportsFilesystemRoot()
    {
        const SocietyContainer container(QDir::rootPath());
        QVERIFY(container.isValid());
        QCOMPARE(container.classifyPath(container.rootPath()), PathKind::Root);
        QCOMPARE(container.classifyPath(root), PathKind::Entry);
    }

private:
    std::unique_ptr<QTemporaryDir> workspace;
    QString root;
    QString sibling;
};

QTEST_GUILESS_MAIN(SocietyContainerTests)
#include "society_container.moc"
