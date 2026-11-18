from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
CASES=['filesStartEmptyAndStayEmptyAcrossReopenAndReplicaCompletion', 'formerDefaultNamesAreOrdinaryUserEntries', 'legacyUpgradeRemovesOnlyEmptyDefaultsOnce', 'legacyReplicasDeferCleanupUntilCompletion', 'legacyUpgradePreservesFilesAndRedirectedEntries', 'projectsFilesAndPreservesPrivateSections', 'rejectsPaths', 'nestedNamesAndHiddenFiles', 'rejectsRedirectionAndDriveReplacement']
ROWS=["../Models/private","folder/../../Models","/Models","C:/Models","folder\\name","a//b","a/./b","a/../b","a/",".","..","nul"]
class FilesViewCTestContract(unittest.TestCase):
    def test_cases_and_data_rows_remain(self):
        text=(ROOT/"tests/files_view.cpp").read_text()
        for case in CASES:self.assertIn(f'runner.run("{case}',text)
        for row in ROWS:self.assertIn(f'rejectsPaths:{row}',text.replace('\\\\','\\'))
        self.assertNotRegex(text,r"\bQTest\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
        self.assertNotIn("SOCIETY_TEST_DIRECTORY",text)
    def test_target_uses_resource_runner_and_no_qttest(self):
        text=(ROOT/"CMakeLists.txt").read_text();link=re.search(r"target_link_libraries\(iiSocietyContainerFilesViewTests\s+([^)]*)\)",text)
        self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1));self.assertIn("files_view_safe",text)
        self.assertNotIn("files-view-qt.txt",text)
if __name__=="__main__":unittest.main()
