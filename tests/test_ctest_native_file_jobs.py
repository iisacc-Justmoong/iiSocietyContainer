from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
CASES=['independentCopiesWaitForTheContainerCommitLock', 'serviceStopInterruptsAWaitingCopyWithoutPublishingIt', 'invalidCommitLockFailsWithoutWaitingIndefinitely', 'copiesExternalAssetAndCompanionsAsDurableJob', 'renamePersistsAndMovesCompanions', 'rejectsRelatedRequestsButNotIndependentRequests', 'protectsInternalMetadata', 'interruptedOperationsRemainUnknownUntilVerified', 'partialResultsRetainTheirActualLocation']
class NativeFileJobsCTestContract(unittest.TestCase):
    def test_cases_and_signals_are_preserved(self):
        source=(ROOT/"tests/native_file_jobs.cpp").read_text()
        for case in CASES:self.assertIn(f'runner.run("{case}"',source)
        self.assertNotRegex(source,r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|<QTest>|<QSignalSpy>|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
    def test_target_does_not_link_qttest(self):
        cmake=(ROOT/"CMakeLists.txt").read_text()
        link=re.search(r"target_link_libraries\(iiSocietyContainerNativeFileJobsTests\s+([^)]*)\)",cmake)
        self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1))
        self.assertIn("iiSocietyContainer.native_file_jobs_protected_metadata",cmake)
if __name__=="__main__":unittest.main()
