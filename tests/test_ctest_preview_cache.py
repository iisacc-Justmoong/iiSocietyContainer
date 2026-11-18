from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ('nativeInformationCardDoesNotHideAnExistingLargeImageDecoder', 'ordinaryFolderImagesHaveReadOnlyPreviewsWithoutACache', 'previewOnlyUsesProviderMediaWithoutHashingTheOriginal', 'embeddingPreservesTheThumbnailRequestKey', 'externalAndRedirectedCachesAreNeverCreated', 'persistentHitAndInvalidation', 'asyncProviderRejectsAnOldRevisionAndSupportsCancellation', 'concurrentRequestsAndCorruptCacheRecover')

class PreviewCacheCTestContract(unittest.TestCase):
    def test_cases_and_completion_signals_remain(self):
        source = (ROOT / "tests/preview_cache.cpp").read_text()
        self.assertEqual(re.findall(r'runner.run\("([^"]+)"', source), list(CASES))
        self.assertIn("std::unique_ptr<FinishedRecorder> complete", source)
        self.assertIn("std::unique_ptr<FinishedRecorder> stale", source)
        self.assertIn("std::vector<std::unique_ptr<FinishedRecorder>> completions", source)
        self.assertIn("CTEST_WAIT_COMPARE(completions[i]->size(), 1)", source)
        self.assertIn("std::unique_ptr<QQuickImageResponse> response", source)
        self.assertIn("retirePreviewResponse(response, complete.get())", source)
        self.assertIn("retirePreviewResponse(response, stale.get())", source)
        self.assertIn("retirePreviewResponse(requests[i]", source)
        guard = (ROOT / "tests/PreviewResponseGuard.h").read_text()
        self.assertIn("response.release()", guard)
        self.assertIn("finished->isEmpty()", guard)
        self.assertIn("const auto restoreCacheDirectory = qScopeGuard", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
        self.assertNotIn("SOCIETY_TEST_DIRECTORY", source)

    def test_runner_and_target(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyContainerPreviewCacheTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("preview_cache_ctest_contract", cmake)
        self.assertIn("iiSocietyContainer.preview_response_lifetime", cmake)
        self.assertRegex(cmake, r"preview_workers_exit COMMAND \${_society_test_runner}")
        self.assertIn("TIMEOUT 20", cmake)
        self.assertIn("foreach(_preview_case IN LISTS _society_preview_cases)", cmake)
        self.assertIn("iiSocietyContainer.preview_cache.${_preview_case}", cmake)
        self.assertIn("$<TARGET_FILE:iiSocietyContainerPreviewCacheTests> ${_preview_case}", cmake)
        for case in CASES:
            self.assertIn(case, cmake)

if __name__ == "__main__":
    unittest.main()
