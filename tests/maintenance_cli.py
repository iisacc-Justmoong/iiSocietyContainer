import json
import select
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

tool, build = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
identifier = "ae1b5aa2-6219-45b5-b63a-70532d790027"
with tempfile.TemporaryDirectory(prefix="maintenance-cli-", suffix=".societycontainer", dir=build) as temporary:
    root = Path(temporary)
    (root / ".society-drive.json").write_text(json.dumps({
        "type": "SocietyDrive", "schemaVersion": 1, "identifier": identifier
    }), encoding="utf-8")
    command = [str(tool), "--root", str(root), "--identifier", identifier]
    empty = json.loads(subprocess.check_output(command + ["--once"]))
    assert empty == {"tempRemoved": 0, "logsRemoved": 0, "cacheRemoved": 0, "bytesRemoved": 0,
                     "skippedBusy": 0, "errors": []}
    assert len(list(root.iterdir())) == 1
    (root / ".society-write-cli").write_text("test", encoding="utf-8")
    once = json.loads(subprocess.check_output(command + ["--once"]))
    assert once["tempRemoved"] == 1 and once["bytesRemoved"] == 4
    for invalid in (["--once", "--watch"], ["--watch", "--interval-seconds", "0"],
                    ["--watch", "--interval-seconds", "300junk"]):
        assert subprocess.run(command + invalid, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE).returncode == 2
    rejected = subprocess.run([str(tool), "--root", str(root), "--identifier",
                               "be1b5aa2-6219-45b5-b63a-70532d790027", "--once"],
                              stdout=subprocess.PIPE)
    assert rejected.returncode == 1 and json.loads(rejected.stdout)["errors"]
    if sys.platform != "win32":
        process = subprocess.Popen(command + ["--watch", "--interval-seconds", "1"],
                                   stdout=subprocess.PIPE, text=True)
        try:
            assert select.select([process.stdout], [], [], 5)[0]
            assert not json.loads(process.stdout.readline())["errors"]
            (root / ".society-write-periodic").write_text("periodic", encoding="utf-8")
            assert select.select([process.stdout], [], [], 5)[0]
            periodic = json.loads(process.stdout.readline())
            assert periodic["tempRemoved"] == 1 and periodic["bytesRemoved"] == 8
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=3) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
print("CLI identity, arguments, empty-store, cleanup, watch, and shutdown passed")
