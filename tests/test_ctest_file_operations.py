from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
CASES=['classificationIsPrivateAcrossFileOperations', 'modelCategoryKeepsPrivateClassification', 'movesProjectPackagesAsSingleItems', 'relocationParticipantRunsWithinTheMove', 'caseOnlyRenameKeepsTheItem', 'relocationFailureReportsWhichSideMoved', 'deletedLocksSeparateIndependentAssetsAndProtectRelatedPaths', 'providerControlsCannotBeManipulatedAsFiles', 'orphanedDestinationMetadataIsNeverAdopted', 'stagesAndRecoversCompleteAssetGroupsAcrossVolumes', 'movesAndRestoresFileClassificationWithTheAsset', 'duplicatesModelAndItsAssociatedAssetWithDistinctIdentity', 'importingModelCopiesRecognizedCompanions', 'nativeRenameNeverReplacesAnExistingItem', 'restoresDeletedItemsWithoutOverwritingAndKeepsOrigins', 'renamedAndLegacyDeletedItemsKeepTrustworthyOrigins', 'rejectsRedirectedRecoveryJournalWithoutMovingSource', 'refusesPartiallyAvailableReplicaWithoutDownloading', 'ordinaryDirectoryWithoutDriveTerminates', 'temporaryLeaseMetadataIsProtectedAndNotCopied', 'createsDirectoriesAndMovesCompleteTrees', 'deletionBypassesContentAndReplicationWork', 'removesPartialPackagesWithoutResolvingMissingChildren', 'handlesFilesPackagesAndCollisions', 'protectsLayoutAndRejectsEscapes']
class FileOperationsCTestContract(unittest.TestCase):
    def test_cases_and_skip_remain(self):
        text=(ROOT/"tests/file_operations.cpp").read_text()
        for case in CASES:self.assertIn(f'runner.run("{case}"',text)
        self.assertIn('society_ctest::skip(',text)
        self.assertNotRegex(text,r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|<QTest>|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
    def test_target_does_not_link_qttest(self):
        text=(ROOT/"CMakeLists.txt").read_text();link=re.search(r"target_link_libraries\(iiSocietyContainerFileOperationsTests\s+([^)]*)\)",text)
        self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1));self.assertIn("file_operations_safe",text)
if __name__=="__main__":unittest.main()
