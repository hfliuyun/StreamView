#!/usr/bin/env python3
"""Run deployed programs without SDK search paths and check required materials."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    environment = os.environ.copy()
    for key in list(environment):
        if key.startswith(("QT_", "QML", "DYLD_", "LD_LIBRARY_PATH")):
            environment.pop(key)
    environment["QT_QPA_PLATFORM"] = "cocoa" if args.platform.startswith("macos") else "offscreen"
    environment["PATH"] = (
        str(Path(os.environ["SystemRoot"]) / "System32")
        if os.name == "nt" else "/usr/bin:/bin"
    )
    if args.platform.startswith("macos"):
        app = root / "StreamView.app"
        gui = app / "Contents/MacOS/streamview"
        materials = root / "StreamView-materials"
        inventory_root = app
        cli = None
    else:
        suffix = ".exe" if os.name == "nt" else ""
        gui = root / "bin" / ("streamview" + suffix)
        cli = root / "bin" / ("svtool" + suffix)
        materials = root if os.name == "nt" else root / "share/streamview"
        inventory_root = root
    for name in ("LICENSE", "LICENSES/Qt-LGPL-3.0-only.txt", "SBOM.json",
                 "README_QT_SOURCE.txt", "QT_ATTRIBUTIONS.json", "QT_SOURCE_ARCHIVES.json"):
        if not (materials / name).is_file():
            raise FileNotFoundError(materials / name)
    sbom = json.loads((materials / "SBOM.json").read_text(encoding="utf-8"))
    if sbom["spdxVersion"] != "SPDX-2.3" or not sbom["files"]:
        raise ValueError("Missing SPDX deployment inventory")
    identifiers = [item["SPDXID"] for item in sbom["packages"] + sbom["files"]]
    if len(set(identifiers)) != len(identifiers):
        raise ValueError("Duplicate SPDX identifiers")
    inventoried = set()
    for item in sbom["files"]:
        relative = Path(item["fileName"])
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"Unsafe SPDX file path: {relative}")
        path = inventory_root / relative
        checksums = {entry["algorithm"]: entry["checksumValue"] for entry in item["checksums"]}
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        if digest.hexdigest() != checksums.get("SHA256"):
            raise ValueError(f"Deployment checksum mismatch: {relative}")
        inventoried.add(path)
    deployed = set()
    for path in inventory_root.rglob("*"):
        if not path.is_file():
            continue
        # Materials are generated after the inventory and cannot inventory themselves.
        if os.name == "nt" and (path == materials / "LICENSE" or
                               path.name in ("SBOM.json", "QT_ATTRIBUTIONS.json",
                                             "QT_SOURCE_ARCHIVES.json", "README_QT_SOURCE.txt") or
                               path.relative_to(materials).parts[0] in ("LICENSES", "SDK_SBOM")):
            continue
        if os.name != "nt" and not args.platform.startswith("macos") and materials in path.parents:
            continue
        deployed.add(path)
    if inventoried != deployed:
        raise ValueError(f"Deployment inventory differs: {sorted(deployed - inventoried)}; "
                         f"missing: {sorted(inventoried - deployed)}")
    paths = [path.name.lower() for path in root.rglob("*") if path.is_file()]
    if not any("qsqlite" in name for name in paths):
        raise FileNotFoundError("Deployed SQLite driver is missing")
    if not any("qoffscreen" in name or "qcocoa" in name or "qwindows" in name
               for name in paths):
        raise FileNotFoundError("Deployed Qt platform plugin is missing")
    with tempfile.TemporaryDirectory(prefix="streamview-package-smoke-") as directory:
        for program, expected in ((gui, f"StreamView {args.version}"), (cli, f"svtool {args.version}")):
            if program is None:
                continue
            result = subprocess.run([str(program), "--version"], cwd=directory,
                                    env=environment, capture_output=True, text=True, timeout=30)
            if result.returncode != 0 or expected not in result.stdout:
                raise RuntimeError(f"{program}: {result.stdout}\n{result.stderr}")
        result = subprocess.run([str(gui), "--check-runtime"], cwd=directory,
                                env=environment, capture_output=True, text=True, timeout=30)
        if result.returncode != 0 or "StreamView runtime OK" not in result.stdout:
            raise RuntimeError(f"Runtime driver/rule check failed: {result.stdout}\n{result.stderr}")
        if cli is not None:
            fixture = Path(directory) / "minimal.264"
            fixture.write_bytes(bytes.fromhex("00000109f0"))
            result = subprocess.run([str(cli), "analyze", str(fixture)], cwd=directory,
                                    env=environment, capture_output=True, text=True, timeout=30)
            if result.returncode != 0 or "NalUnitHeader" not in result.stdout:
                raise RuntimeError(f"Bundled rule analysis failed: {result.stdout}\n{result.stderr}")
    print(f"Package smoke passed: {args.platform} {args.version}")


if __name__ == "__main__":
    main()
