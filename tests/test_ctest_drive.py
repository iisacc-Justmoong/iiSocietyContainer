from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "replicaIdentityUpdatesWhileMaintenanceReadsTheManifest",
    "nativeAndQtUnicodeParentsPreserveThePackageBoundary",
    "bareFoldersImagesAndRedirectedPackagesAreRejectedWithoutWrites",
    "createsPackageStorageTreeAndReusesIdentity",
    "refusesConflictingOrRedirectedStorageFolder",
    "accessesLogicalSectionTreesAndRejectsReplacedIdentity",
    "createsAndReopensDriveWithNineIndependentRoots",
    "migratesLegacyPhotosWithPreviewsAndIdentity",
    "legacyNameKeepsIdentityAndSourceData",
    "adoptsHostIdentityWithoutMovingSectionsOrLosingData",
    "keepsExistingContentAndRejectsDirectoryConflictsBeforeCreation",
    "preservesUnassignedFiles",
    "rejectsNestedContainersBeforeWriting",
    "rejectsFinderReplicasAndTheirDescendants",
    "doesNotInferAnUninitializedDrive",
    "rejectsInvalidManifestWithoutOverwritingIt",
    "detectsMissingSectionsAndInvalidSectionValues",
    "preventsSectionsFromAliasingEachOther",
)


class DriveCTestContract(unittest.TestCase):
    def test_cases_rows_and_fixture_remain(self):
        source = (ROOT / "tests/society_drive.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runCase("{case}"', source)
        for row in ("identical-duplicate", "conflicting-content"):
            self.assertIn(f'runCase("legacyPhotosMergePreservesConflictsAndReplicaState:{row}"', source)
        self.assertIn("tests.init()", source)
        self.assertIn("society_ctest::skip(", source)
        self.assertIn("QDir::current()", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_source_and_installed_consumer_use_resource_runner(self):
        for file, target in (("CMakeLists.txt", "iiSocietyContainerDriveTests"),
                             ("tests/consumer/CMakeLists.txt", "iiSocietyContainerDriveConsumer")):
            cmake = (ROOT / file).read_text()
            link = re.search(r"target_link_libraries\(" + target + r"\s+([^)]*)\)", cmake)
            self.assertIsNotNone(link)
            self.assertNotIn("Qt6::Test", link.group(1))
            self.assertIn("SKIP_RETURN_CODE 77", cmake)
        source = (ROOT / "CMakeLists.txt").read_text()
        installed = (ROOT / "tests/consumer/CMakeLists.txt").read_text()
        self.assertIn("drive_ctest_contract", source)
        self.assertRegex(source, r"drive COMMAND \${_society_test_runner}")
        self.assertRegex(installed, r"installed_drive COMMAND \${_society_consumer_runner}")
        self.assertIn("installed_drive_safe", installed)


if __name__ == "__main__":
    unittest.main()
