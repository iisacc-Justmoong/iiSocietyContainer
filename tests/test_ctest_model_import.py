from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "repairsMissingCategoriesWhenAContainerIsSelected",
    "catalogOnlyModelSearchAndIdentityConflictsNeedNoObjectStore",
    "importsDirectlyWithoutWaitingForOrDuplicatingTheObjectStore",
    "staleEmptyOperationLockDoesNotBlockMultipleImports",
    "liveOperationLockIsNotRemovedAndWaitingIsVisible:identified owner",
    "liveOperationLockIsNotRemovedAndWaitingIsVisible:empty but natively locked",
    "publicationTimeoutPreservesRetryableBatchWithoutCopyingTheRest",
    "publicationWaitCanBeCancelledWithoutTouchingTheOwnerOrSource",
    "multipleLargeModelsCrossCopyCheckpointsAndKeepTheirBytes",
    "importKeepsMetadataWithTheContainerObject",
    "invalidPortableMetadataDoesNotPublishTheModel",
    "concurrentImportsKeepEachModelsOwnMetadata",
    "backgroundOrganizationDoesNotEraseAUserError",
    "importsAndRecoversJobHistoryWithoutAnApplicationAdapter",
)

class ModelImportCTestContract(unittest.TestCase):
    def test_cases_rows_skip_and_signals_remain(self):
        source = (ROOT / "tests/model_import_service.cpp").read_text()
        self.assertEqual(re.findall(r'runner.run\("([^\"]+)"', source), list(CASES))
        for name in ("organized", "finished", "recategorized", "firstDone", "secondDone"):
            self.assertIn(f"society_ctest::SignalRecorder {name}", source)
        self.assertIn("society_ctest::skip(", source)
        self.assertIn("#if !defined(Q_OS_WIN) && !defined(Q_OS_ANDROID)", source)
        self.assertIn("SOCIETY_JOB_STATE_ROOT", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_selection(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyContainerModelImportServiceTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiSocietyContainer.model_import_offline", cmake)
        self.assertIn("iiSocietyContainer.model_import_ctest_contract", cmake)

if __name__ == "__main__":
    unittest.main()
