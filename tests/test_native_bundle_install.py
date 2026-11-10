"""The SDK installer must never silently downgrade the mounted-drive host."""
import sys
import tempfile
import unittest
from unittest.mock import patch
import os
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "platform/macos"))
from install_native_bundle import install_bundle
from build_native import (clear_unprovisioned_filesystem, replace_generated_directory,
                          provisioning_options, validate_signing_configuration)


class NativeBundleInstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / "source/Society.app"
        self.destination = self.root / "installed/Society.app"
        self.source.mkdir(parents=True)
        self.destination.mkdir(parents=True)
        (self.source / "new").write_text("new")
        (self.destination / "old").write_text("old")

    def tearDown(self):
        self.temp.cleanup()

    def install(self, source, previous):
        def inspect(path):
            return source if path == self.source else previous
        install_bundle(self.source, self.destination, inspect=inspect, verify=lambda _: None)

    def test_unprovisioned_build_cannot_replace_existing_filesystem_module(self):
        with self.assertRaisesRegex(ValueError, "FSKit"):
            self.install((False, False, False), (True, False, True))
        self.assertEqual((self.destination / "old").read_text(), "old")
        self.assertFalse((self.destination / "new").exists())

    def test_partial_or_unentitled_source_is_rejected(self):
        for source in ((True, False, True), (True, True, False), (False, True, False)):
            with self.subTest(source=source), self.assertRaisesRegex(ValueError, "FSKit"):
                self.install(source, (False, False, False))
        self.assertTrue((self.destination / "old").exists())

    def test_matching_capability_replaces_bundle_without_stale_files(self):
        self.install((True, True, True), (True, True, True))
        self.assertEqual((self.destination / "new").read_text(), "new")
        self.assertFalse((self.destination / "old").exists())

    def test_non_filesystem_build_can_update_an_unprovisioned_bundle(self):
        self.install((False, False, False), (False, False, False))
        self.assertEqual((self.destination / "new").read_text(), "new")
        self.assertFalse((self.destination / "old").exists())

    def test_failed_publication_restores_the_previous_bundle(self):
        rename = os.rename
        calls = []
        def fail_publication(source, destination):
            calls.append((source, destination))
            if len(calls) == 2:
                raise OSError("publication failed")
            return rename(source, destination)
        with patch("install_native_bundle.os.rename", side_effect=fail_publication):
            with self.assertRaisesRegex(OSError, "publication failed"):
                self.install((True, True, True), (True, True, True))
        self.assertEqual((self.destination / "old").read_text(), "old")

    def test_failed_rollback_preserves_the_previous_bundle_in_the_transaction(self):
        rename = os.rename
        calls = []
        def fail_publication_and_restore(source, destination):
            calls.append((source, destination))
            if len(calls) >= 2:
                raise OSError("blocked")
            return rename(source, destination)
        with patch("install_native_bundle.os.rename", side_effect=fail_publication_and_restore):
            with self.assertRaisesRegex(RuntimeError, "previous bundle"):
                self.install((True, True, True), (True, True, True))
        backups = list(self.destination.parent.glob(".society-native-install-*/previous.app/old"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_text(), "old")

    def test_unprovisioned_rebuild_removes_old_generated_capability(self):
        extension = self.source / "Contents/Extensions/SocietyDriveFS.appex"
        extension.mkdir(parents=True)
        (extension / "stale").write_text("stale")
        profile = self.source / "Contents/embedded.provisionprofile"
        profile.write_text("stale")
        clear_unprovisioned_filesystem(self.source)
        self.assertFalse(extension.exists())
        self.assertFalse(profile.exists())

    def test_unprovisioned_rebuild_rejects_a_redirected_extension(self):
        extension = self.source / "Contents/Extensions/SocietyDriveFS.appex"
        extension.parent.mkdir(parents=True)
        outside = self.root / "outside"; outside.mkdir()
        (outside / "keep").write_text("keep")
        extension.symlink_to(outside, target_is_directory=True)
        with self.assertRaises(ValueError):
            clear_unprovisioned_filesystem(self.source)
        self.assertEqual((outside / "keep").read_text(), "keep")

    def test_fskit_never_uses_an_adhoc_host_and_offline_build_does_not_update_the_portal(self):
        with self.assertRaisesRegex(ValueError, "signing"):
            validate_signing_configuration("TEAM", "-")
        validate_signing_configuration("TEAM", "Apple Development: Example")
        validate_signing_configuration("", "-")
        self.assertEqual(provisioning_options(True), [])
        self.assertEqual(provisioning_options(False), ["-allowProvisioningUpdates"])

    def test_regenerated_filesystem_module_does_not_retain_stale_files(self):
        source = self.root / "new-extension"; source.mkdir()
        destination = self.root / "old-extension"; destination.mkdir()
        (source / "new").write_text("new")
        (destination / "old").write_text("old")
        replace_generated_directory(source, destination)
        self.assertEqual((destination / "new").read_text(), "new")
        self.assertFalse((destination / "old").exists())


if __name__ == "__main__":
    unittest.main()
