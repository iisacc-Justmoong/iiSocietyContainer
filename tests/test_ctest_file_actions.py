from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "pathCapabilitiesFollowTheCurrentSelection",
    "unrelatedDeletesAreAcceptedAndCompleteWhileAnotherItemIsBusy",
    "closingAnAdapterDoesNotCancelAnAcceptedRemoval",
    "renamesLocalModelWithoutDownloadingOrReadingPayload",
    "renameKeepsTrackedIdentityAndDefersChangedContent",
    "renameChangesOnlyLetterCaseOnCaseInsensitiveStorage",
    "renameMovesModelPackagesAndKeepsTrackedChildren",
    "renameWaitsForTheIndexWithoutMovingTheFileEarly",
    "renameRejectsRemoteOnlyAndReplicaFilesWithoutDownloading",
    "deletionNeverQueuesDownloads",
    "copiesUrlsAndPastesWithoutOverwriting",
    "metadataMenuDoesNotPullAndCopyWaitsForSelectedObject",
)


class FileActionsCTestContract(unittest.TestCase):
    def test_cases_signal_arguments_and_limits_remain(self):
        source = (ROOT / "tests/file_actions.cpp").read_text()
        self.assertEqual(re.findall(r'runner.run\("([^"]+)"', source), list(CASES))
        for name in ("finished", "completed", "failed", "done"):
            self.assertIn(f"society_ctest::SignalRecorder {name}", source)
        self.assertIn("event.at(2)", source)
        self.assertIn("society_ctest::skip(", source)
        for timeout in ("1500", "15000", "30000", "120000"):
            self.assertIn(timeout, source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_uses_resource_runner_without_qttest(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyContainerFileActionsTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertRegex(cmake, r"file_actions COMMAND \${_society_test_runner}")
        self.assertIn("file_actions_ctest_contract", cmake)
        self.assertRegex(cmake, r"file_actions PROPERTIES[^)]*SKIP_RETURN_CODE 77")


if __name__ == "__main__":
    unittest.main()
