from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "libraryDoesNotImportSharedMountBuffer",
    "concurrentContainerLookupCoexistsWithOtherMountQueries",
    "ordinaryPackageLookupsDoNotPollDiskImages",
    "savedImageSettingsNeverMountOrRenameThePackage",
    "mountedImageCannotReplaceOrdinaryFolderSettings",
    "lowLevelImageMigrationNeverMakesItAManagedPackage",
    "preservesConflictingImagesAndRejectsOrdinaryFolders",
)


class DiskImageCTestContract(unittest.TestCase):
    def test_cases_remain_without_qttest(self):
        source = (ROOT / "tests/disk_image.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|<QTest>|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
        self.assertNotIn("SOCIETY_TEST_DIRECTORY", source)

    def test_resource_runner_and_target(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyContainerDiskImageTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiSocietyContainer.disk_image_safe", cmake)
        self.assertRegex(cmake, r"iiSocietyContainer\.disk_image COMMAND \$\{_society_test_runner\}")


if __name__ == "__main__":
    unittest.main()
