#!/usr/bin/env python3
"""Install an exact native bundle without dropping a provisioned FSKit host."""
import argparse
import os
import plistlib
import shutil
import subprocess
import tempfile
from pathlib import Path


def entitlement(bundle, key):
    result = subprocess.run(["/usr/bin/codesign", "-d", "--entitlements", ":-", str(bundle)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode or not result.stdout:
        return False
    try:
        return plistlib.loads(result.stdout).get(key) is True
    except (ValueError, plistlib.InvalidFileException):
        return False


def signing_team(bundle):
    result = subprocess.run(["/usr/bin/codesign", "-dvv", str(bundle)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, text=True)
    if result.returncode or "Signature=adhoc" in result.stderr:
        return ""
    for line in result.stderr.splitlines():
        if line.startswith("TeamIdentifier="):
            team = line.partition("=")[2].strip()
            return "" if team == "not set" else team
    return ""


def inspect_bundle(bundle):
    extension = bundle / "Contents/Extensions/SocietyDriveFS.appex"
    if extension.is_symlink() or (extension.exists() and not extension.is_dir()):
        raise ValueError("The FSKit module is invalid or redirected")
    present = extension.is_dir()
    host_team = signing_team(bundle)
    host_profile = bundle / "Contents/embedded.provisionprofile"
    module_profile = extension / "Contents/embedded.provisionprofile"
    host_ready = bool(host_team and host_profile.is_file() and not host_profile.is_symlink()
                      and entitlement(bundle, "com.apple.developer.fskit.mount"))
    module_ready = bool(present and host_team and signing_team(extension) == host_team
                        and module_profile.is_file() and not module_profile.is_symlink()
                        and entitlement(extension, "com.apple.developer.fskit.fsmodule"))
    return present, host_ready, module_ready


def verify_bundle(bundle):
    subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(bundle)], check=True)


def install_bundle(source, destination, *, inspect=inspect_bundle, verify=verify_bundle):
    source, destination = Path(source), Path(destination)
    if source.is_symlink() or not source.is_dir() or destination.is_symlink():
        raise ValueError("The native Society bundle is missing or redirected")
    if source.resolve() == destination.resolve():
        raise ValueError("The source and installation bundle must differ")
    if destination.exists() and not destination.is_dir():
        raise ValueError("The native Society installation is not a directory")
    module, host_entitled, module_entitled = inspect(source)
    if module != host_entitled or module != module_entitled:
        raise ValueError("The FSKit module and host capabilities do not match")
    if destination.exists():
        previous_module, previous_host, _ = inspect(destination)
        if (previous_module or previous_host) and not module:
            raise ValueError("Refusing to replace an existing FSKit host with an unprovisioned build")
    verify(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    transaction = Path(tempfile.mkdtemp(prefix=".society-native-install-", dir=destination.parent))
    preserve = False
    try:
        staged, backup = transaction / "new.app", transaction / "previous.app"
        shutil.copytree(source, staged, symlinks=True)
        verify(staged)
        had_previous = destination.exists()
        if had_previous:
            os.rename(destination, backup)
        try:
            os.rename(staged, destination)
        except BaseException as error:
            if had_previous:
                try:
                    os.rename(backup, destination)
                except OSError as rollback:
                    preserve = True
                    raise RuntimeError(f"Could not restore the previous bundle; it remains at {backup}: {rollback}") from error
            raise
    finally:
        if not preserve:
            shutil.rmtree(transaction)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    try:
        install_bundle(args.source, args.destination)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Society native bundle installation failed: {error}\n")


if __name__ == "__main__":
    main()
