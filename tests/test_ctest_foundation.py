from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ['repeatedSnapshotsKeepRevisionAndChangedInputsAreStillValidated', 'stateIsAnAliasSnapshotAndDoesNotProbeOrCreateStorage', 'aliasesFromAnotherContainerAreNeverAccepted', 'lookupFollowsReplacementFilteringAndReentrantReaders', 'repeatedLookupCostIsMeasuredWithoutStorageWork']

class CTestFoundationContracts(unittest.TestCase):
    def test_alias_cases_and_background_child_are_preserved(self):
        alias = (ROOT / "tests/container_alias.cpp").read_text()
        background = (ROOT / "tests/directory_background.cpp").read_text()
        for name in CASES:
            self.assertIn(f'runner.run("{name}"', alias)
        self.assertIn('runner.run("applicationWaitsForDetachedViewWork"', background)
        self.assertIn('"--child"', background)
        self.assertNotIn("QDir::current().filePath", background)
        for source in (alias, background):
            self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|<QTest>|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_targets_do_not_link_qttest(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        for target in ("iiSocietyContainerAliasTests", "iiSocietyContainerDirectoryBackgroundTests"):
            link = re.search(r"target_link_libraries\(" + target + r"\s+([^)]*)\)", cmake)
            self.assertIsNotNone(link)
            self.assertNotIn("Qt6::Test", link.group(1))

if __name__ == "__main__":
    unittest.main()
