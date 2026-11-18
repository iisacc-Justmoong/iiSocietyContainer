from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "providerMetadataAndOntologyAreTheActualSearchSource",
    "documentsExposeRevisionedPreviewsWithoutBecomingImages",
    "providerControlDirectoryContentsNeverBecomeRecentFiles",
    "editorCanvasesDoNotDuplicateGeneratedImagesInHistory",
    "concurrentSectionsKeepMetadataIndependent",
    "historyIsPersistedBoundedAndObserved",
    "onlyHistoryLinksAreAccepted",
)


class DashboardCTestContract(unittest.TestCase):
    def test_cases_signals_and_resources_are_preserved(self):
        source = (ROOT / "tests/dashboard.cpp").read_text()
        self.assertEqual(re.findall(r'runner.run\("([^"]+)"', source), list(CASES))
        self.assertIn("society_ctest::SignalRecorder changed", source)
        self.assertIn("society_ctest::SignalRecorder requested", source)
        self.assertIn("CTEST_WAIT_COMPARE_TIMEOUT", source)
        self.assertIn("30000", source)
        self.assertIn("QDir::currentPath()", source)
        self.assertNotIn("SOCIETY_TEST_DIRECTORY", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_uses_resource_runner_without_qttest(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyContainerDashboardTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertRegex(cmake, r"dashboard COMMAND \${_society_test_runner}")
        self.assertIn("dashboard_ctest_contract", cmake)


if __name__ == "__main__":
    unittest.main()
