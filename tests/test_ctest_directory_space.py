from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "accessesMetadataAndNestedTree", "acceptsDirectoryWithoutChangingContents",
    "keepsSpacesIndependent", "normalizesAndPinsRelativeRoot",
    "acceptsUnicodeAndWhitespaceInDirectoryNames", "resolvesSymbolicLinksBeforeClassification",
    "detectsRemovedOrRedirectedRoot", "exposesExactlyTheNineLogicalSections",
    "logicalSectionsAreIndependentOfPhysicalLayout", "rejectsUnknownSectionIdentifiers",
    "supportsFilesystemRoot",
)
ROWS = {
    "rejectsInvalidRoots": ("empty", "missing", "file", "file-as-parent", "embedded-null", "qt-resource"),
    "classifiesRelativePaths": ("root", "normalized-root", "directory", "file", "parent", "same-prefix-sibling", "traversal", "missing", "empty", "embedded-null", "qt-resource"),
}


class DirectorySpaceCTestContract(unittest.TestCase):
    def test_cases_data_and_fixtures_remain(self):
        source = (ROOT / "tests/society_container.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        for case, rows in ROWS.items():
            for row in rows:
                self.assertIn(f'runner.run("{case}:{row}"', source)
        self.assertIn("tests.init()", source)
        self.assertIn("society_ctest::skip(", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_uses_resource_runner_without_qttest(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyContainerTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiSocietyContainer.directory_space_ctest_contract", cmake)
        self.assertRegex(cmake, r"iiSocietyContainer\.directory_space COMMAND \$\{_society_test_runner\}")

    def test_installed_consumers_use_resource_runner_without_qttest(self):
        cmake = (ROOT / "tests/consumer/CMakeLists.txt").read_text()
        for target in ("Directory", "Logical", "Files", "Operations"):
            link = re.search(rf"target_link_libraries\(iiSocietyContainer{target}Consumer\s+([^)]*)\)", cmake)
            self.assertIsNotNone(link)
            self.assertNotIn("Qt6::Test", link.group(1))
        for name in ("directory_space", "logical_storage", "files_projection", "file_operations"):
            self.assertRegex(cmake, rf"iiSocietyContainer\.installed_{name} COMMAND \$\{{_society_consumer_runner\}}")
        operations = re.search(r"add_executable\(iiSocietyContainerOperationsConsumer\s+([^)]*)\)", cmake)
        self.assertIsNotNone(operations)
        for source in ("ExclusiveRename.cpp", "AssetTransfer.cpp", "DeletedOrigins.cpp"):
            self.assertIn(source, operations.group(1))
        self.assertIn('target_include_directories(iiSocietyContainerOperationsConsumer PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../..")', cmake)


if __name__ == "__main__":
    unittest.main()
