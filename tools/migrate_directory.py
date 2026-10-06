#!/usr/bin/env python3
"""Copy a quiescent legacy Society drive into a verified ordinary directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import time

OS_ENTRIES = {".DS_Store", ".fseventsd", ".DocumentRevisions-V100", ".TemporaryItems", ".Trashes"}
# Large reads reduce read/write seeks when the old image and new folder share a disk.
COPY_BLOCK = 128 * 1024 * 1024


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def inventory(data_root, files_root):
    manifest = json.loads((data_root / ".society-drive.json").read_text())
    if manifest.get("type") != "SocietyDrive" or len(manifest.get("sections", [])) != 9:
        raise ValueError("A valid nine-section Society drive is required")
    entries, directories = [], []
    roots = [(p, Path(p.name)) for p in data_root.iterdir()
             if p.name not in OS_ENTRIES | {".society-disk.plist", ".society-drive.lock", "Files"}]
    roots.append((files_root, Path("Files")))
    def visit(source, relative):
        if source.is_symlink():
            raise ValueError(f"Redirected entry cannot be migrated: {source}")
        stat = source.stat()
        if source.is_dir():
            directories.append(str(relative))
            for child in sorted(source.iterdir()):
                if child.name not in OS_ENTRIES:
                    visit(child, relative / child.name)
        elif source.is_file():
            entries.append({"source": str(source), "path": str(relative), "bytes": stat.st_size,
                            "mtime_ns": stat.st_mtime_ns, "inode": stat.st_ino})
        else:
            raise ValueError(f"Unsupported filesystem entry: {source}")
    for source, relative in roots:
        visit(source, relative)
    return manifest, sorted(entries, key=lambda e: (e["path"] == ".society-drive.json", e["bytes"])), directories


def copy_directory_metadata(data_root, files_root, destination, directories):
    for relative in sorted(directories, key=lambda p: len(Path(p).parts), reverse=True):
        path = Path(relative)
        source = files_root.joinpath(*path.parts[1:]) if path.parts[0] == "Files" else data_root / path
        shutil.copystat(source, destination / path)
    shutil.copystat(data_root, destination)


def migrate(data_root, files_root, destination, report_directory):
    data_root, files_root = data_root.resolve(strict=True), files_root.resolve(strict=True)
    if destination.exists() or destination.is_symlink():
        raise ValueError("The destination already exists; it will not be replaced")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.parent.resolve() != destination.parent:
        raise ValueError("The destination parent must not be redirected")
    if destination == data_root or data_root in destination.parents or files_root in destination.parents:
        raise ValueError("The destination must be outside both legacy volumes")
    report_directory.mkdir(parents=True, exist_ok=True)
    manifest, entries, directories = inventory(data_root, files_root)
    signature = {"source": str(data_root), "files_source": str(files_root),
                 "destination": str(destination), "identifier": manifest["identifier"], "entries": entries}
    state_path = report_directory / "copy-state.json"
    staging = destination.with_name("." + destination.name + ".migrating")
    if state_path.exists():
        state = json.loads(state_path.read_text())
        if state["inventory"] != signature:
            raise ValueError("The source inventory changed; resume refused")
    else:
        if staging.exists():
            raise ValueError("Unknown staging directory exists")
        state = {"inventory": signature, "verified": {}}
        write_json(state_path, state)
    ledger = report_directory / "verified-files.jsonl"
    if ledger.exists():
        for line in ledger.read_text().splitlines():
            row = json.loads(line)
            state["verified"][row["path"]] = row["sha256"]
    staging.mkdir(exist_ok=True)
    if staging.is_symlink():
        raise ValueError("Staging must not be redirected")
    for directory in directories:
        (staging / directory).mkdir(parents=True, exist_ok=True)
    # Private state remains owner-only while the payload is copied.
    copy_directory_metadata(data_root, files_root, staging, directories)
    total = sum(e["bytes"] for e in entries)
    completed = sum(e["bytes"] for e in entries if e["path"] in state["verified"])
    print(json.dumps({"phase": "copy", "files": len(entries), "bytes": total, "completed": completed}), flush=True)
    last_report = time.monotonic()
    for entry in entries:
        source, target = Path(entry["source"]), staging / entry["path"]
        if entry["path"] in state["verified"]:
            # Reverify completed files on resume before trusting the ledger.
            digest = hashlib.sha256()
            with target.open("rb") as stream:
                while chunk := stream.read(4 * 1024 * 1024): digest.update(chunk)
            actual = digest.hexdigest()
            if target.stat().st_size != entry["bytes"] or actual != state["verified"][entry["path"]]:
                raise ValueError(f"Completed file changed: {target}")
            continue
        partial = target.with_name(target.name + ".society-migration-part")
        source_hash, target_hash = hashlib.sha256(), hashlib.sha256()
        written = 0
        with source.open("rb", buffering=0) as input_stream, partial.open("w+b", buffering=0) as output:
            while chunk := input_stream.read(COPY_BLOCK):
                source_hash.update(chunk)
                offset = output.tell()
                view = memoryview(chunk)
                while view:
                    count = output.write(view)
                    if not count: raise OSError("Short write")
                    view = view[count:]
                readback = os.pread(output.fileno(), len(chunk), offset)
                if readback != chunk: raise OSError(f"Copy verification failed: {source}")
                target_hash.update(readback)
                written += len(chunk)
                if time.monotonic() - last_report >= 30:
                    print(json.dumps({"phase": "copy", "path": entry["path"], "completed": completed + written,
                                      "bytes": total}), flush=True)
                    last_report = time.monotonic()
            os.fsync(output.fileno())
        current = source.stat()
        if written != entry["bytes"] or current.st_size != entry["bytes"] or current.st_mtime_ns != entry["mtime_ns"]:
            raise ValueError(f"Source changed while copying: {source}")
        if source_hash.digest() != target_hash.digest(): raise ValueError("Content hash mismatch")
        shutil.copystat(source, partial)
        partial.replace(target)
        completed += written
        state["verified"][entry["path"]] = source_hash.hexdigest()
        with ledger.open("a") as stream:
            stream.write(json.dumps({"path": entry["path"], "sha256": source_hash.hexdigest()}) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
    if inventory(data_root, files_root)[1] != entries:
        raise ValueError("Source changed before publication")
    for section in manifest["sections"]:
        if not (staging / section["path"]).is_dir(): raise ValueError("Missing section")
    # File creation changes parent timestamps; restore them only after copying.
    copy_directory_metadata(data_root, files_root, staging, directories)
    # The identity and logical paths are preserved; only disk-specific metadata is omitted.
    staging.rename(destination)
    report = {"status": "verified", "identifier": manifest["identifier"], "destination": str(destination),
              "files": len(entries), "bytes": total, "sha256": state["verified"],
              "source_preserved": True, "settings_changed": False}
    write_json(report_directory / "verification.json", report)
    print(json.dumps({k: v for k, v in report.items() if k != "sha256"}), flush=True)
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("data-root", "files-root", "destination", "report-directory"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    migrate(args.data_root, args.files_root, args.destination.absolute(), args.report_directory)
