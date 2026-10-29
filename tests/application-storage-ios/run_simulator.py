#!/usr/bin/env python3
"""Run bootstrap/adopt/reopen in two real iOS app processes on a fresh simulator."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--owner", required=True, type=Path)
    parser.add_argument("--adoption", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    def run(*command, check=True):
        result = subprocess.run(command, text=True, capture_output=True, timeout=180)
        if check and result.returncode:
            raise RuntimeError(f"{' '.join(command)}\n{result.stdout}\n{result.stderr}")
        return result

    runtimes = json.loads(run("xcrun", "simctl", "list", "runtimes", "-j").stdout)["runtimes"]
    runtime = next(r["identifier"] for r in runtimes if r["isAvailable"] and r.get("platform") == "iOS")
    device = run("xcrun", "simctl", "create", "iiSociety logical storage probe",
                 "com.apple.CoreSimulator.SimDeviceType.iPhone-18-Pro", runtime).stdout.strip()
    (args.output / "simulator.json").write_text(json.dumps({"device": device, "runtime": runtime}, indent=2))
    results = []
    try:
        print(f"Booting isolated simulator {device}", flush=True)
        run("xcrun", "simctl", "boot", device)
        run("xcrun", "simctl", "bootstatus", device, "-b")
        absent = run("xcrun", "simctl", "get_app_container", device, "com.iisacc.society", check=False)
        if absent.returncode == 0:
            raise RuntimeError("Society must not be installed in the bootstrap environment")
        for app in (args.owner, args.adoption):
            run("codesign", "--force", "--sign", "-", "--entitlements",
                str(Path(__file__).with_name("App.entitlements")), str(app))
            run("xcrun", "simctl", "install", device, str(app))
        for role, bundle, flags in (
            ("bootstrap", "com.iisacc.tests.logical-owner", []),
            ("adopt", "com.iisacc.tests.logical-adoption", ["--adopt"]),
            ("reopen", "com.iisacc.tests.logical-owner", ["--reopen"]),
        ):
            print(f"Running {role}", flush=True)
            launched = run("xcrun", "simctl", "launch", "--console", "--terminate-running-process", device, bundle, *flags, check=False)
            (args.output / f"{role}.log").write_text(launched.stdout + launched.stderr)
            records = [json.loads(line.split("STORAGE_PROBE ", 1)[1]) for line in launched.stdout.splitlines() if "STORAGE_PROBE " in line]
            if not records or records[-1].get("ok") is not True:
                raise RuntimeError(f"{role} failed: {launched.stdout}\n{launched.stderr}")
            result = records[-1]
            if result["action"] != role or result["societyInstalled"]:
                raise RuntimeError("Unexpected application or bootstrap state")
            if results and (result["root"] != results[0]["root"] or result["registry"]["storageId"] != results[0]["registry"]["storageId"]):
                raise RuntimeError("Adoption changed the physical source or identity")
            results.append(result)
        (args.output / "result.json").write_text(json.dumps({"ok": True, "device": device, "runtime": runtime, "steps": results}, indent=2))
        print("Shared App Group bootstrap, cross-app adoption and reopen passed", flush=True)
    finally:
        run("xcrun", "simctl", "shutdown", device, check=False)
        run("xcrun", "simctl", "delete", device)


if __name__ == "__main__":
    main()
