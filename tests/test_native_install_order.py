"""An unprovisioned full SDK install must not partially overwrite an FSKit SDK."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit("Expected build directory and cmake executable")
BUILD_PATH, CMAKE = Path(sys.argv[1]), sys.argv[2]


class FullInstallSafetyTests(unittest.TestCase):
    def test_native_preflight_precedes_all_other_sdk_files(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / "sdk"
            old = prefix / "share/iiSocietyContainer/Society.app/Contents/Extensions/SocietyDriveFS.appex"
            old.mkdir(parents=True)
            (old / "preserve").write_text("old filesystem")
            include = prefix / "include"
            include.mkdir()
            (include / "preserve.h").write_text("old sdk")
            before = sorted(str(path.relative_to(prefix)) for path in prefix.rglob("*"))
            result = subprocess.run([CMAKE, "--install", str(BUILD_PATH), "--prefix", str(prefix)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("FSKit", result.stdout)
            self.assertEqual(before, sorted(str(path.relative_to(prefix)) for path in prefix.rglob("*")))
            self.assertEqual((old / "preserve").read_text(), "old filesystem")
            self.assertEqual((include / "preserve.h").read_text(), "old sdk")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
