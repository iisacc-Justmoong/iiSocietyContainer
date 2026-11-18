from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SINGLE = (
    "categoryMoveKeepsPortablePresentationWithTheModel", "renameKeepsCompanionsAliasesAndManualCategory",
    "providerMetadataIsNeverOrganizedAsAModel", "reconcilesLegacyMoveWithMetadataStillAtTheOldPath",
    "repairsLegacyCategoryMoveUsingJournalAndNativeIdentity", "legacyRepairRefusesUnverifiedOrConflictingMetadata",
    "reportsDetachedClassificationWhenCatalogIsMissing", "movesPortableMetadataAndPreservesConflictingCompanions",
    "manualOtherSurvivesOrganizationAndSubsequentMoves", "exposesBoundedModelMetadataForCatalogs",
    "createsAndRepairsEveryCategoryWithoutChangingTheManifest", "keepsAllCategoriesAndExistingUserContent",
    "layoutConflictPreservesTheExistingEntryAndOtherMissingCategories", "explicitMetadataCoversEveryType",
    "validatesLargeSparseSafetensorsWithoutReadingPayload", "animaRequiresConsistentBackboneAndAdapter",
    "acceptsScalarEmptyAndPackedTensors", "reorganizesPreviouslyUnknownAnimaAndPreservesReferences",
    "packagesDocumentsAndGguf", "boundedInspectionDoesNotGuessFromNamesOrExecuteLegacyFiles",
    "organizesPackagesAndCompanionsWithoutReplacingFilesOrReferences", "pathReferencesTravelWithTheContainer",
    "rejectsRedirectedCategoriesInvalidJournalsAndOutsideMoves",
)
VALIDATION_ROWS = (
    "unknown-dtype", "missing-dtype", "negative-shape", "fractional-shape", "overflow-shape",
    "missing-shape", "wrong-shape-length", "negative-offset", "fractional-offset", "reversed-offset",
    "outside-payload", "missing-offset", "invalid-descriptor", "truncated-data", "unindexed-data",
    "overlapping-data", "gap-in-data", "non-string-metadata", "invalid-metadata", "metadata-without-tensors",
    "duplicate-tensor", "duplicate-field", "escaped-duplicate-field",
)
SIGNATURE_ROWS = ("checkpoint", "lora", "lycoris", "dora-before-lora", "controlnet", "motion",
    "embedding", "vae", "upscaler", "unet", "text-encoder", "clip-vision", "clip", "llm", "vlm",
    "unknown-is-not-a-checkpoint")

class ModelStoreCTestContract(unittest.TestCase):
    def test_single_cases_and_data_rows_remain(self):
        source = (ROOT / "tests/model_store.cpp").read_text()
        for name in SINGLE:
            self.assertTrue(f'runner.run("{name}"' in source, f"missing case: {name}")
        for name in VALIDATION_ROWS + SIGNATURE_ROWS:
            self.assertIn(f'"{name}"', source)
        for row in ("kreaCheckpoint:native", "kreaCheckpoint:comfy", "animaCheckpoint:unwrapped",
                    "animaCheckpoint:base-net", "animaCheckpoint:finetune", "animaCheckpoint:complete"):
            self.assertIn(f'runner.run("{row}"', source)
        self.assertIn('"validatesTensorDescriptorsAndPayload:" + row.first', source)
        self.assertIn('"tensorSignatures:" + row.name', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_source_and_consumer_targets_use_ctest(self):
        for filename, target in (("CMakeLists.txt", "iiSocietyContainerModelStoreTests"),
                                 ("tests/consumer/CMakeLists.txt", "iiSocietyContainerModelsConsumer")):
            cmake = (ROOT / filename).read_text()
            link = re.search(r"target_link_libraries\(" + target + r"\s+([^)]*)\)", cmake)
            self.assertIsNotNone(link)
            self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiSocietyContainer.model_store_invalid_tensors", (ROOT / "CMakeLists.txt").read_text())
        self.assertIn("iiSocietyContainer.installed_models", (ROOT / "tests/consumer/CMakeLists.txt").read_text())

if __name__ == "__main__":
    unittest.main()
