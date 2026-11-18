from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
CASES=['nativeDesktopRootIsIndependentOfApplicationMetadata', 'suiteDiscoveryIsIndependentOfTheProductAndReadersDoNotBootstrap', 'explicitSelectionsAndInvalidSuiteRootsNeverForkStorage', 'redirectedSuiteRootsNeverTouchAnotherDirectory', 'sharedGuardIsPrivateAndRejectsRedirection', 'applicationOwnsPersistentLogicalDiskWithoutAPackageOrSociety', 'societyAdoptsTheSameDiskAndPreservesPathsIdentityReferencesAndManagedHistory', 'existingPackageWinsWithoutCreatingAnotherDisk', 'hostIdentityChangesKeepTheRegistryAndManagedOriginalsAttached', 'malformedRegistrationAndConflictingFilesNeverCreateAReplacement']
class LogicalStorageCTestContract(unittest.TestCase):
    def test_cases_and_skip_paths_are_preserved(self):
        text=(ROOT/"tests/logical_storage.cpp").read_text()
        for case in CASES:self.assertIn(f'runner.run("{case}"',text)
        self.assertNotRegex(text,r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|<QTest>|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
        self.assertGreaterEqual(text.count("society_ctest::skip("),3)
        self.assertNotIn("LOGICAL_STORAGE_TEST_DIRECTORY",text)
    def test_resource_runner_and_qttest_link(self):
        cmake=(ROOT/"CMakeLists.txt").read_text()
        link=re.search(r"target_link_libraries\(iiSocietyContainerLogicalStorageTests\s+([^)]*)\)",cmake)
        self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1))
        self.assertIn("iiSocietyContainer.logical_storage_safe",cmake)
        self.assertNotIn('LOGICAL_STORAGE_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}"',cmake)
if __name__=="__main__":unittest.main()
