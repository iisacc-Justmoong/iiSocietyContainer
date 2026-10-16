#!/usr/bin/env python3
"""Explicit real FSKit integration: requires the signed, enabled Society module.

Run with drive-tool, native-host, and SDK build directory. General CTest does
not register or enable OS extensions. Original fixtures stay under build/;
macOS 27 creates its own /Volumes mount namespace through FSClient.
The fixture is removed only after actual unmount has succeeded.
"""
import hashlib
import errno
import fcntl
import json
import os
import platform
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor

tool, native, build = (str(Path(argument).resolve()) for argument in sys.argv[1:])
volume_metadata = (".fseventsd", ".Spotlight-V100")
user_operation_names = (".Trashes", ".TemporaryItems", ".DocumentRevisions-V100")
existing_user_names = ("existing-user-document.txt", *volume_metadata, ".DS_Store", ".TemporaryItems")

def reject_metadata_mutation(action):
    try:
        action()
    except OSError as failure:
        assert failure.errno == errno.EPERM, (failure.errno, failure)
    else:
        raise AssertionError("A protected metadata mutation must not alter user Files")

def call(action, source, success=True):
    result = subprocess.run([native, action, str(source)], text=True, capture_output=True, timeout=40)
    value = json.loads(result.stdout)
    assert (result.returncode == 0) == success, (result.returncode, value, result.stderr)
    return value

temporary = tempfile.mkdtemp(prefix="filesystem-mount-", dir=build)
mounted = False
try:
    source = Path(temporary) / "Society.societycontainer"
    source.mkdir()
    info = json.loads(subprocess.check_output([tool, "create", str(source)], text=True))
    manifest = source / ".society-drive.json"
    before = hashlib.sha256(manifest.read_bytes()).hexdigest()
    (source / "Models/private.txt").write_text("private")
    (source / "Files/from-app.txt").write_text("from-app")
    (source / "Files/.iisacc-temporary.lock").write_bytes(b"")
    (source / "Files/allocated.bin").write_bytes(bytes(range(251)) * 262)
    with (source / "Files/sparse.bin").open("wb") as stream:
        stream.seek((1 << 30) - 1)
        stream.write(b"x")
        stream.flush()
        os.fsync(stream.fileno())
    (source / "Files/private-link").symlink_to(source / "Models", target_is_directory=True)
    mounted = False
    try:
        result = call("mount", source)
        mounted = True
        point = Path(result["systemPath"])
        if int(platform.mac_ver()[0].split(".")[0]) >= 27:
            assert point.parent == Path("/Volumes"), "FSClient must create the OS-managed mount namespace"
            assert not (source / ".society-runtime/mounts").exists(), "Society must not prepare backing directories for the public macOS 27 API"
        else:
            assert point == source / ".society-runtime/mounts" / info["identifier"] / "Society", "The macOS 26 backing directory must stay inside the package"
        assert result["isMounted"] and result["filesystemType"] == "societyfs"
        assert result["identifier"] == info["identifier"]
        assert point.stat().st_dev != source.stat().st_dev, "Must be an actual OS mount"
        mounted_device = point.stat().st_dev
        for name in volume_metadata:
            reject_metadata_mutation(lambda name=name: (point / name).mkdir())
            assert not (source / "Files" / name).exists(), "A rejected management folder must leave no source entry"
        for name in user_operation_names:
            operation_folder = point / name
            existed_before = operation_folder.exists()
            try:
                operation_folder.mkdir(exist_ok=True)
            except OSError as failure:
                if name != ".DocumentRevisions-V100" or failure.errno != errno.EPERM:
                    raise
                assert not existed_before, "Only a new volume-root revision-store request may report this OS limitation"
                assert not os.path.lexists(source / "Files" / name), "OS rejection must not leave any source entry"
                print(json.dumps({"osLimitation": "volume-root-document-revisions-create",
                                  "errno": "EPERM", "callerPid": os.getpid(), "sourceEntryCreated": False}))
                continue
            operation_file = operation_folder / "retained-user.txt"
            operation_file.write_bytes(b"user recovery and save state")
            assert operation_file.read_bytes() == b"user recovery and save state"
            assert (source / "Files" / name / "retained-user.txt").read_bytes() == b"user recovery and save state"
            operation_file.unlink()
            assert not os.path.lexists(source / "Files" / name / "retained-user.txt")
            if not existed_before and not list(operation_folder.iterdir()):
                operation_folder.rmdir()
            print(json.dumps({"nativeUserOperation": "volume-root-file-crud", "name": name,
                              "result": "passed", "callerPid": os.getpid()}))
        reject_metadata_mutation(lambda: (point / ".DS_Store").write_bytes(b"new Finder store"))
        assert not (source / "Files/.DS_Store").exists()
        rename_source = point / "metadata-rename-source"
        rename_source.write_bytes(b"keep source on rejection")
        for name in (*volume_metadata, ".DS_Store"):
            reject_metadata_mutation(lambda name=name: rename_source.rename(point / name))
            assert rename_source.read_bytes() == b"keep source on rejection"
            assert not (source / "Files" / name).exists(), "Rename rejection must leave no destination"
        rename_source.unlink()
        assert (point / "from-app.txt").read_text() == "from-app"
        sentinel_name = ".iisacc-temporary.lock"
        assert sentinel_name not in {entry.name for entry in point.iterdir()}
        assert not (point / sentinel_name).exists(), "Lease metadata lookup must stay private"
        try:
            (point / sentinel_name).write_bytes(b"replace")
        except OSError:
            pass
        else:
            raise AssertionError("Public writes must protect the lease sentinel")
        assert (source / "Files" / sentinel_name).read_bytes() == b""
        for name in ("from-app.txt", "allocated.bin", "sparse.bin"):
            original = (source / "Files" / name).stat()
            projected = (point / name).stat()
            assert projected.st_size == original.st_size, "Projection must preserve logical file size"
            assert projected.st_blocks == original.st_blocks, "Projection must preserve allocated 512-byte sectors"
        assert (point / "sparse.bin").stat().st_blocks * 512 < 1 << 30, "Sparse allocation must remain sparse"
        assert "private-link" not in {entry.name for entry in point.iterdir()}
        assert not (point / "private-link").exists()
        for hidden in ("Models", "Photos", "Files", ".society-drive.json", ".society-runtime"):
            assert not (point / hidden).exists(), hidden
        directory = point / "한글 folder"
        directory.mkdir()
        reject_metadata_mutation(lambda: (directory / ".DS_Store").write_bytes(b"new nested Finder store"))
        assert not (source / "Files/한글 folder/.DS_Store").exists()
        for name in volume_metadata:
            nested_user_folder = directory / name
            nested_user_folder.mkdir()
            nested_payload = nested_user_folder / "user.txt"
            nested_payload.write_bytes(b"nested user data")
            assert (source / "Files/한글 folder" / name / "user.txt").read_bytes() == b"nested user data"
            nested_payload.unlink(); nested_user_folder.rmdir()
        file = directory / "document #1.txt"
        file.write_bytes(b"from-mounted-drive")
        assert (source / "Files/한글 folder/document #1.txt").read_bytes() == b"from-mounted-drive"
        with file.open("r+b") as stream:
            stream.seek(5); stream.write(b"UPDATED"); stream.truncate(12)
        renamed = point / "renamed.txt"
        assert file.exists() and (source / "Files/한글 folder/document #1.txt").exists()
        file.rename(renamed)
        assert renamed.read_bytes() == b"from-UPDATED"
        renamed.unlink(); directory.rmdir()
        lease_folder = point / "lease-only"
        lease_folder.mkdir()
        original_folder = source / "Files/lease-only"
        original_sentinel = original_folder / sentinel_name
        original_sentinel.write_bytes(b"")
        assert list(lease_folder.iterdir()) == [], "A sentinel-only folder must appear empty"
        with original_sentinel.open("r+b") as writer:
            fcntl.flock(writer, fcntl.LOCK_SH)
            try:
                lease_folder.rmdir()
            except OSError as failure:
                assert failure.errno == errno.EBUSY, "Active staging must prevent folder removal"
            else:
                raise AssertionError("An active writer's folder was removed")
            assert original_sentinel.exists(), "Rejected deletion must preserve its sentinel inode"
            fcntl.flock(writer, fcntl.LOCK_UN)
        lease_folder.rmdir()
        assert not original_folder.exists(), "Explicit deletion must remove an idle sentinel-only folder"
        assert call("mount", source)["systemPath"] == str(point), "Reconnect must reuse the mount"
        moved_source = source.with_name("Renamed.societycontainer")
        source.rename(moved_source)
        try:
            renamed_result = call("mount", moved_source)
            assert renamed_result["identifier"] == info["identifier"]
            assert renamed_result["systemPath"] == str(point), "A renamed source must find the mounted UUID"
            assert (point / "from-app.txt").read_text() == "from-app"
        finally:
            moved_source.rename(source)
        assert hashlib.sha256(manifest.read_bytes()).hexdigest() == before
        assert (source / "Models/private.txt").read_text() == "private"
        assert call("unmount", source)["isMounted"] is False
        mounted = False
        assert not point.exists() or point.stat().st_dev != mounted_device, "Unmount must remove the actual filesystem"
        original_user_fixtures = {}
        for name in existing_user_names:
            original = source / "Files" / name
            if name == ".TemporaryItems":
                original.mkdir()
                original = original / "user.txt"
            original.write_bytes(b"pre-existing user file")
            original_user_fixtures[name] = (original.read_bytes(), original.stat().st_dev, original.stat().st_ino)
        existing_metadata_folder = source / "Files/existing-user-metadata"
        existing_metadata_folder.mkdir()
        (existing_metadata_folder / ".DS_Store").write_bytes(b"pre-existing nested user file")
        protected_metadata_folder = source / "Files/existing-user-protected/.DS_Store"
        protected_metadata_folder.mkdir(parents=True)
        protected_original = protected_metadata_folder / "user.txt"
        protected_original.write_bytes(b"pre-existing protected child")
        protected_identity = (protected_original.stat().st_dev, protected_original.stat().st_ino)
        result = call("mount", source)
        mounted = True
        assert result["identifier"] == info["identifier"]
        point = Path(result["systemPath"])
        assert (point / "from-app.txt").read_text() == "from-app"
        listed = {entry.name for entry in point.iterdir()}
        original_listed = {entry.name for entry in (source / "Files").iterdir()}
        print(json.dumps({"nativeExistingUserListing": {"callerPid": os.getpid(),
                         "sourcePath": str(source), "systemPath": str(point),
                         "expectedNames": list(existing_user_names), "sourceNames": sorted(original_listed),
                         "projectedNames": sorted(listed),
                         "missingNames": sorted(set(existing_user_names) - listed)}}), flush=True)
        def concurrent_listing(index):
            concurrent_names = {entry.name for entry in point.iterdir()}
            source_names = {entry.name for entry in (source / "Files").iterdir()}
            print(json.dumps({"nativeConcurrentUserListing": {"request": index, "thread": threading.get_ident(),
                              "callerPid": os.getpid(), "projectedNames": sorted(concurrent_names),
                              "sourceNames": sorted(source_names),
                              "missingNames": sorted(set(existing_user_names) - concurrent_names),
                              "missingSourceNames": sorted(set(existing_user_names) - source_names)}}), flush=True)
            return index, concurrent_names
        with ThreadPoolExecutor(max_workers=4) as readers:
            for request, concurrent_names in readers.map(concurrent_listing, range(12)):
                assert set(existing_user_names) <= concurrent_names, f"Concurrent listing {request} lost existing user names: {sorted(set(existing_user_names) - concurrent_names)!r}; projected={sorted(concurrent_names)!r}"
                assert ".iisacc-temporary.lock" not in concurrent_names, "Every listing must keep the lease sentinel private"
        for name in existing_user_names:
            assert name in listed, f"Matching pre-existing user file {name!r} must remain listed; projected={sorted(listed)!r}; source={sorted(original_listed)!r}"
            user_file = point / name
            original = source / "Files" / name
            if name == ".TemporaryItems":
                assert user_file.is_dir(), "Matching pre-existing user folders must remain accessible"
                user_file = user_file / "user.txt"
                original = original / "user.txt"
            original_bytes = original.read_bytes()
            original_identity = (original.stat().st_dev, original.stat().st_ino)
            assert (original_bytes, *original_identity) == original_user_fixtures[name], "The pre-mount user bytes and identity must remain intact"
            read_blocked = False
            print(json.dumps({"nativeExistingUserOperation": {"name": name, "operation": "read",
                              "path": str(user_file), "callerPid": os.getpid()}}), flush=True)
            try:
                projected_bytes = user_file.read_bytes()
            except OSError as failure:
                if failure.errno != errno.EPERM:
                    raise
                if name == ".Spotlight-V100" and user_file == point / ".Spotlight-V100":
                    limitation = "existing-volume-root-spotlight-read"
                elif name == ".TemporaryItems" and user_file == point / ".TemporaryItems/user.txt":
                    limitation = "existing-volume-root-temporary-items-read"
                else:
                    raise
                assert original.read_bytes() == original_bytes, "OS read rejection must preserve original user bytes"
                assert (original.stat().st_dev, original.stat().st_ino) == original_identity, "OS read rejection must preserve the original user file"
                print(json.dumps({"osLimitation": limitation,
                                  "errno": "EPERM", "callerPid": os.getpid(), "sourceUserBytesUnchanged": True,
                                  "sourceUserIdentityUnchanged": True,
                                  "projectionWriteChecked": False}))
                read_blocked = True
            if not read_blocked:
                assert projected_bytes == original_bytes, "Matching files must remain readable"
            if name in (*volume_metadata, ".DS_Store"):
                reject_metadata_mutation(lambda: user_file.write_bytes(b"must preserve original"))
                reject_metadata_mutation(lambda: user_file.unlink())
                reject_metadata_mutation(lambda: user_file.rename(point / "must-not-move-metadata"))
                reject_metadata_mutation(lambda: os.chmod(user_file, 0o600))
                assert (original.read_bytes(), original.stat().st_dev, original.stat().st_ino) == original_user_fixtures[name], "Read-only metadata protection must preserve the pre-mount original"
                print(json.dumps({"nativeExistingMetadataProtection": {"name": name, "result": "passed",
                                  "sourceUserBytesUnchanged": True, "sourceUserIdentityUnchanged": True}}), flush=True)
                continue
            if read_blocked:
                continue
            for revision in range(3):
                updated = f"updated user file {revision}".encode()
                print(json.dumps({"nativeExistingUserOperation": {"name": name, "operation": "write",
                                  "revision": revision, "path": str(user_file), "callerPid": os.getpid()}}), flush=True)
                user_file.write_bytes(updated)
                assert user_file.read_bytes() == updated, "Read-then-write must remain usable through the projection"
                assert original.read_bytes() == updated, "Matching files must remain writable"
                assert (original.stat().st_dev, original.stat().st_ino) == original_identity, "Writing must preserve the supplied user inode"
        replacement = point / "metadata-replacement"
        replacement.write_bytes(b"replace an existing user file")
        reject_metadata_mutation(lambda: replacement.rename(point / ".DS_Store"))
        assert replacement.read_bytes() == b"replace an existing user file"
        replacement.unlink()
        assert (source / "Files/.DS_Store").read_bytes() == b"pre-existing user file"
        nested_user_file = point / "existing-user-metadata/.DS_Store"
        assert nested_user_file.read_bytes() == b"pre-existing nested user file"
        reject_metadata_mutation(lambda: nested_user_file.write_bytes(b"must preserve nested user file"))
        reject_metadata_mutation(lambda: nested_user_file.unlink())
        assert (existing_metadata_folder / ".DS_Store").read_bytes() == b"pre-existing nested user file"
        protected_folder = point / "existing-user-protected/.DS_Store"
        protected_file = protected_folder / "user.txt"
        assert "user.txt" in {entry.name for entry in protected_folder.iterdir()}
        assert protected_file.read_bytes() == b"pre-existing protected child"
        reject_metadata_mutation(lambda: protected_folder.rename(point / "must-not-move-protected-folder"))
        reject_metadata_mutation(lambda: os.chmod(protected_folder, 0o700))
        reject_metadata_mutation(lambda: protected_file.write_bytes(b"must preserve protected child"))
        reject_metadata_mutation(lambda: protected_file.unlink())
        reject_metadata_mutation(lambda: protected_file.rename(protected_folder / "renamed.txt"))
        reject_metadata_mutation(lambda: os.chmod(protected_file, 0o600))
        reject_metadata_mutation(lambda: (protected_folder / "new-child.txt").write_bytes(b"must not create"))
        reject_metadata_mutation(lambda: (protected_folder / "new-folder").mkdir())
        assert protected_original.read_bytes() == b"pre-existing protected child"
        assert (protected_original.stat().st_dev, protected_original.stat().st_ino) == protected_identity
        assert {entry.name for entry in protected_metadata_folder.iterdir()} == {"user.txt"}
        assert hashlib.sha256(manifest.read_bytes()).hexdigest() == before
        invalid = Path(temporary) / "plain-folder"
        invalid.mkdir()
        call("mount", invalid, success=False)
        assert list(invalid.iterdir()) == [], "A rejected source must not be initialized"
        assert call("unmount", source)["isMounted"] is False
        mounted = False
        for name in (*volume_metadata, ".DS_Store"):
            original = source / "Files" / name
            assert (original.read_bytes(), original.stat().st_dev, original.stat().st_ino) == original_user_fixtures[name]
            original.write_bytes(b"source user edit")
            assert original.read_bytes() == b"source user edit"
            moved = original.with_name(original.name + ".source-user-edit")
            original.rename(moved)
            moved.rename(original)
            assert (original.stat().st_dev, original.stat().st_ino) == original_user_fixtures[name][1:]
            original.write_bytes(original_user_fixtures[name][0])
        protected_original.write_bytes(b"source protected child edit")
        assert protected_original.read_bytes() == b"source protected child edit"
        print("Existing reserved user data remained read-only through the mount and editable through its authoritative source.", flush=True)
    finally:
        if mounted:
            call("unmount", source)
            mounted = False
        # /Volumes entries belong to macOS; the test never mkdir/rmdir there.
finally:
    actual_mounts = subprocess.check_output(["/sbin/mount"], text=True)
    if mounted or source.as_uri() + "/" in actual_mounts:
        print(json.dumps({"retainedMountedFixture": temporary}), flush=True)
    else:
        shutil.rmtree(temporary)
print("FSKit actual mount, Files boundary, metadata creation rejection, existing user data, CRUD, reconnect and identity passed.")
