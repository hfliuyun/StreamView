#!/usr/bin/env python3
"""Download and verify versioned Qt source archives for release materials."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import urllib.request


def download(url, destination):
    with urllib.request.urlopen(url, timeout=60) as response, destination.open("wb") as target:
        shutil.copyfileobj(response, target)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    base = (f"https://download.qt.io/official_releases/qt/"
            f"{args.version.rsplit('.', 1)[0]}/{args.version}/submodules")
    evidence = []
    for module in ("qtbase", "qtsvg"):
        name = f"{module}-everywhere-src-{args.version}.tar.xz"
        archive = args.output / name
        checksum = args.output / (name + ".sha256")
        download(f"{base}/{name}.sha256", checksum)
        expected = checksum.read_text().split()[0]
        if len(expected) != 64 or any(c not in "0123456789abcdef" for c in expected):
            raise ValueError("Invalid upstream SHA256")
        if not archive.exists():
            download(f"{base}/{name}", archive)
        with archive.open("rb") as stream:
            digest = hashlib.sha256()
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        if digest.hexdigest() != expected:
            raise ValueError(f"Checksum mismatch: {archive}")
        evidence.append({"module": module, "url": f"{base}/{name}", "sha256": expected})
        destination = args.output / module
        if destination.exists():
            raise FileExistsError(destination)
        destination.mkdir()
        with tarfile.open(archive) as source:
            for member in source:
                relative = Path(*Path(member.name).parts[1:])
                if relative.is_absolute() or ".." in relative.parts:
                    raise ValueError(f"Unsafe archive member: {member.name}")
                if member.isfile():
                    target = destination / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with source.extractfile(member) as stream, target.open("wb") as output:
                        shutil.copyfileobj(stream, output)
    (args.output / "sources.json").write_text(
        json.dumps(evidence, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
