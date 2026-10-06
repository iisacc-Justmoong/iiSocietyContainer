import importlib.util
import json
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("migration", Path(__file__).parents[1] / "tools/migrate_directory.py")
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


class MigrationTests(unittest.TestCase):
    def test_interrupted_copy_resumes_with_completed_hashes(self):
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            root = Path(temporary)
            data, files, destination = root / "data", root / "public", root / "Society"
            data.mkdir(); files.mkdir()
            sections = ["Files", "Photos", "Asset Library", "Generation History", "Models",
                        "Thinking Space", "Forked", "Published", "Deleted"]
            for name in sections[1:]: (data / name).mkdir()
            (data / ".society-drive.json").write_text(json.dumps({"type": "SocietyDrive", "identifier": "resume-id",
                "sections": [{"path": p} for p in sections]}))
            (files / "a").write_bytes(b"a")
            (files / "b").write_bytes(b"bb")
            pread = migration.os.pread
            calls = 0
            def interrupted(*args):
                nonlocal calls
                calls += 1
                if calls == 2: raise OSError("simulated interruption")
                return pread(*args)
            with patch.object(migration.os, "pread", side_effect=interrupted):
                with self.assertRaises(OSError): migration.migrate(data, files, destination, root / "report")
            self.assertFalse(destination.exists())
            report = migration.migrate(data, files, destination, root / "report")
            self.assertEqual(report["identifier"], "resume-id")
            self.assertEqual((destination / "Files/a").read_bytes(), b"a")
            self.assertEqual((destination / "Files/b").read_bytes(), b"bb")

    def test_preserves_identity_payload_hidden_state_and_separate_files(self):
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            root = Path(temporary)
            data, files, destination = root / "data", root / "public", root / "Society"
            data.mkdir(); files.mkdir()
            sections = ["Files", "Photos", "Asset Library", "Generation History", "Models",
                        "Thinking Space", "Forked", "Published", "Deleted"]
            manifest = {"type": "SocietyDrive", "identifier": "kept-id", "sections": [{"path": p} for p in sections]}
            (data / ".society-drive.json").write_text(json.dumps(manifest))
            for name in sections[1:]: (data / name).mkdir()
            (data / "Models/model.bin").write_bytes(b"weights" * 1000)
            (files / "document.txt").write_text("public file")
            (data / ".society-sync").mkdir()
            (data / ".society-sync/catalog.json").write_text("index")
            (data / ".society-sync").chmod(0o700)
            os.utime(data / ".society-sync", ns=(1234567890000000000, 1234567890000000000))
            (data / ".society-disk.plist").write_text("legacy disk")
            report = migration.migrate(data, files, destination, root / "report")
            self.assertEqual(report["identifier"], "kept-id")
            self.assertEqual((destination / "Files/document.txt").read_text(), "public file")
            self.assertEqual((destination / "Models/model.bin").read_bytes(), b"weights" * 1000)
            self.assertEqual((destination / ".society-sync/catalog.json").read_text(), "index")
            self.assertEqual(stat.S_IMODE((destination / ".society-sync").stat().st_mode), 0o700)
            self.assertEqual((destination / ".society-sync").stat().st_mtime_ns,
                             (data / ".society-sync").stat().st_mtime_ns)
            self.assertFalse((destination / ".society-disk.plist").exists())
            self.assertTrue((data / ".society-disk.plist").exists())
            self.assertEqual(json.loads((destination / ".society-drive.json").read_text()), manifest)
            with self.assertRaises(ValueError): migration.migrate(data, files, destination, root / "report")

    def test_redirected_source_is_rejected_before_copy(self):
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            root = Path(temporary)
            data, files = root / "data", root / "files"
            data.mkdir(); files.mkdir()
            (data / ".society-drive.json").write_text(json.dumps({"type": "SocietyDrive", "identifier": "id",
                                                               "sections": [{"path": str(i)} for i in range(9)]}))
            (data / "link").symlink_to(files)
            with self.assertRaises(ValueError): migration.migrate(data, files, root / "Society", root / "report")
            self.assertFalse((root / ".Society.migrating").exists())


if __name__ == "__main__": unittest.main()
