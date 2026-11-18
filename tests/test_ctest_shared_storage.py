from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SINGLE = ('legacyClassificationsAreIsolatedWithoutMergingUnknownFiles', 'replacedExternalFileDoesNotInheritLegacyClassification', 'stalePrivateClassificationDoesNotAttachToAReusedPath', 'directorySearchAndTypeSortUseCurrentOptions', 'modelDirectoryHidesAttachedCompanionsInLiveAndCachedRows', 'replicaModelDirectoryHidesCompanionsWithoutDownloading', 'replicaModelTreeNavigatesCatalogDirectoriesWithoutDownloading', 'replicaModelTreeRejectsDeletedAndRedirectedFolders', 'replicaModelTreeRechecksDeletedDescendantsBeforeOpening', 'staleDirectorySnapshotNeverExposesProviderMetadata', 'daemonInventoryRestoresBeforeLiveValidation', 'modelInventoryExcludesOnnxAndKeepsGguf', 'fileInformationExposesProviderPresentationAndClassification', 'externalFilesReceiveProviderDetailsAndPreviewWithoutAnAppSave', 'generatedPngPublicationPreservesBytesAndRejectsCollisionsAndRedirects', 'runtimePathsStayInsideTheSelectedPackageAndCreateOnlyOnRequest', 'cachedDirectorySnapshotIsReconciledBeforeOpening', 'generationInventoryReconcilesAuthorityAndPreservesReplicas', 'originalModelFilesRemainVisibleWithAPartialCatalogAndNoRegisteredHost', 'localModelReferenceRemainsStableWhileProviderPublishesPreviews', 'modelTypesMatchPhysicalFoldersAndRemoteCatalog', 'directoryRefreshPublishesOnlyTheChangedRows', 'hostListingsFollowDeletionBeforeTheCatalogCatchesUp', 'largeDirectoryRefreshReturnsBeforeReadingTheStorageMap', 'largeReplicaModelTreeListsOneCategoryWithoutOpeningPayloads', 'directoryListsRemoteObjectsAndOpensOnlyCompletePayloads', 'navigationDiscardsAStaleDirectoryRead', 'modelCardsCreateOnlyTheSelectedDownloadRequest', 'missingModelPackageIsNotReportedReady', 'remoteModelCatalogDoesNotCreatePayloads', 'defaultSelectionFollowsHostAdoptionAndConsumersWaitForReadiness', 'rejectsSavedImageSettingsWithoutChangingFilesOrConfiguration', 'rejectsMisplacedContainersWithoutReplacingSharedSettings', 'nativePathsSupportOrdinaryFilesInEverySection', 'nativePathsRejectTraversalAndRedirection', 'pinnedStorageRejectsRootRedirectAndIdentityReplacement', 'appsDiscoverTheSameDriveAndCannotSilentlySwitchIdentity', 'referencesResolveOnlyWithinTheSameModelsArea', 'privateOutputDirectoriesRejectRedirection')
ROWS = (
    "emptyModelCategoriesAppearInLocalAndReplicaListings:local",
    "emptyModelCategoriesAppearInLocalAndReplicaListings:replica",
    "replicaCatalogDoesNotExposeUnindexedLocalModels:incomplete",
    "replicaCatalogDoesNotExposeUnindexedLocalModels:complete",
    "modelCatalogExcludesProviderMetadata:filesystem",
    "modelCatalogExcludesProviderMetadata:local-authority",
    "modelCatalogExcludesProviderMetadata:remote-replica",
    "modelCatalogIgnoresUnchangedRefreshAndDocumentation:filesystem",
    "modelCatalogIgnoresUnchangedRefreshAndDocumentation:replica-catalog",
)

class SharedStorageCTestContract(unittest.TestCase):
    def test_original_cases_rows_and_model_contract_remain(self):
        source = (ROOT / "tests/shared_storage.cpp").read_text()
        for name in SINGLE + ROWS:
            self.assertTrue(f'runner.run("{name}"' in source, f"missing case: {name}")
        self.assertIn('"hostModelImportsAppearBeforeSyncHashingCompletes:" + std::to_string(iteration)', source)
        self.assertIn("iteration < 16", source)
        self.assertIn("society_ctest::FlatModelContract modelContract(model)", source)
        self.assertIn("modelContract.ok()", source)
        self.assertIn("society_ctest::SignalCounter resets", source)
        self.assertIn("callbackChecks.rethrow()", source)
        self.assertIn("society_ctest::skip(", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_no_qttest_package_or_consumer_link(self):
        for filename in ("CMakeLists.txt", "tests/consumer/CMakeLists.txt"):
            text = (ROOT / filename).read_text()
            self.assertFalse("Qt6::Test" in text, f"QtTest link remains in {filename}")
            self.assertFalse(re.search(r"find_package\(Qt6[^\n]*COMPONENTS[^\n]*\bTest\b", text), f"QtTest package remains in {filename}")
        self.assertIn("iiSocietyContainer.shared_storage_model_contract", (ROOT / "CMakeLists.txt").read_text())

if __name__ == "__main__":
    unittest.main()
