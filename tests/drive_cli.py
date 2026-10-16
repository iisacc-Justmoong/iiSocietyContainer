"""New drive creation through the public CLI; no native mount or UI is used."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid


tool = Path(sys.argv[1]).resolve(strict=True)
build = Path(sys.argv[2]).resolve(strict=True)
sections = {
    "Files", "Photos", "Asset Library", "Generation History", "Models",
    "Thinking Space", "Forked", "Published", "Deleted",
}


def snapshot(root):
    """Keep bytes as well as paths so rejected requests cannot rewrite data."""
    result = {}
    for path in root.rglob("*"):
        relative = path.relative_to(root).as_posix()
        assert not path.is_symlink(), "The fixture must not contain redirects"
        result[relative] = ("directory", b"") if path.is_dir() else ("file", path.read_bytes())
    return result


with tempfile.TemporaryDirectory(prefix="drive-cli-", dir=str(build)) as temporary:
    work = Path(temporary)

    def call(action, target, success=True):
        completed = subprocess.run(
            [str(tool), action, str(target)], cwd=str(work),
            capture_output=True, text=True, encoding="utf-8", timeout=30,
        )
        assert (completed.returncode == 0) == success, (
            action, str(target), completed.returncode, completed.stdout, completed.stderr,
        )
        data = json.loads(completed.stdout)
        assert isinstance(data, dict), data
        if success:
            assert "error" not in data, data
        else:
            assert data.get("error"), data
        return data

    # The caller supplies an existing ordinary parent. The CLI must create the
    # package itself, without a preliminary mkdir of Society.societycontainer.
    parent = work / "사용자 parent #100%"
    parent.mkdir()
    sentinel = parent / "기존 사용자.txt"
    sentinel_bytes = "사용자가 넣은 원문\n".encode("utf-8") + b"\x00\xff"
    sentinel.write_bytes(sentinel_bytes)
    package = parent / "Society.societycontainer"
    expected_root = package.as_posix()
    assert not package.exists()

    created = call("create", parent)
    assert created["rootPath"] == expected_root, created
    assert created["displayName"] == "Society", created
    identifier = created["identifier"]
    assert str(uuid.UUID(identifier)) == identifier, created
    assert package.is_dir()
    assert not (parent / "Society").exists()
    assert not (package / "bands").exists()
    assert {entry.name for entry in parent.iterdir()} == {package.name, sentinel.name}
    assert sentinel.read_bytes() == sentinel_bytes

    manifest_bytes = (package / ".society-drive.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    assert manifest["type"] == "SocietyDrive" and manifest["schemaVersion"] == 1
    assert manifest["identifier"] == identifier
    assert len(manifest["sections"]) == 9
    assert {section["path"] for section in manifest["sections"]} == sections
    assert {entry.name for entry in package.iterdir()} == sections | {".society-drive.json"}
    for section in sections:
        assert (package / section).is_dir(), section
    assert not list((package / "Models").iterdir())

    preserved = snapshot(parent)
    for action, target in (("create", parent), ("create", package), ("open", package)):
        reopened = call(action, target)
        assert reopened["rootPath"] == expected_root and reopened["identifier"] == identifier, reopened
        assert snapshot(parent) == preserved, "Reopening must retain user bytes and the minimal layout"

    # Rejection must neither create a fallback package nor modify caller data.
    relative = work / "relative-parent"
    relative.mkdir()
    before = snapshot(work)
    call("create", relative.name, success=False)
    assert snapshot(work) == before, "A relative parent must be rejected without writes"

    missing = work / "missing-parent"
    before = snapshot(work)
    call("create", missing, success=False)
    assert not missing.exists() and snapshot(work) == before, "A missing parent must remain missing"

    conflict_parent = work / "conflicting-parent"
    conflict_parent.mkdir()
    conflict = conflict_parent / "Society.societycontainer"
    conflict_bytes = b"existing caller file\x00\xff"
    conflict.write_bytes(conflict_bytes)
    before = snapshot(work)
    call("create", conflict_parent, success=False)
    assert conflict.is_file() and conflict.read_bytes() == conflict_bytes
    assert snapshot(work) == before, "A conflicting file must be preserved without other writes"

print("Drive CLI creates one minimal package from a Unicode parent, preserves identity and rejects invalid parents without writes")
