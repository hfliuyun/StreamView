#!/usr/bin/env python3
"""Create deterministic compliance files from a staged deployment tree."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil


def checksums(path: Path) -> list[dict[str, str]]:
    digests = {"SHA1": hashlib.sha1(), "SHA256": hashlib.sha256()}
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            for digest in digests.values():
                digest.update(block)
    return [{"algorithm": name, "checksumValue": digest.hexdigest()}
            for name, digest in digests.items()]


def copy_required(source: Path, destination: Path) -> None:
    if not source.is_file():
        raise FileNotFoundError(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--qt-version", required=True)
    parser.add_argument("--qt-prefix", required=True, type=Path)
    parser.add_argument("--qt-source", required=True, type=Path)
    parser.add_argument("--epoch", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    stage = args.stage.resolve(strict=True)
    qt_prefix = args.qt_prefix.resolve(strict=True)
    qt_source = args.qt_source.resolve(strict=True)
    output = args.output.resolve()
    if output == stage or stage in output.parents:
        raise ValueError("Compliance output must be outside the deployment tree")
    output.mkdir(parents=True, exist_ok=True)
    licenses = output / "LICENSES"
    copy_required(root / "LICENSE", output / "LICENSE")
    copy_required(root / "LICENSE", licenses / "StreamView-MIT.txt")
    copy_required(root / "third_party/tomlplusplus/LICENSE",
                  licenses / "tomlplusplus-MIT.txt")

    qt_license = qt_source / "qtbase/LICENSES/LGPL-3.0-only.txt"
    copy_required(qt_license, licenses / "Qt-LGPL-3.0-only.txt")
    attributions = []
    for module in ("qtbase", "qtsvg"):
        module_source = qt_source / module
        copy_required(module_source / "LICENSES/LGPL-3.0-only.txt",
                      licenses / module / "LICENSES/LGPL-3.0-only.txt")
        for path in sorted(module_source.rglob("*")):
            if path.is_file() and (path.name.upper().startswith(
                    ("LICENSE", "COPYING", "NOTICE", "COPYRIGHT")) or
                    "LICENSES" in path.parts):
                copy_required(path, licenses / path.relative_to(qt_source))
        for manifest in sorted((module_source / "src").rglob("qt_attribution.json")):
            # Qt attribution files can contain literal newlines in copyright text.
            declarations = json.loads(manifest.read_text(encoding="utf-8"), strict=False)
            if isinstance(declarations, dict):
                declarations = [declarations]
            for declaration in declarations:
                relative = manifest.relative_to(qt_source).as_posix()
                attributions.append({"sourceManifest": relative, **declaration})
                copy_required(manifest, licenses / relative)
                license_files = declaration.get("LicenseFile", [])
                if isinstance(license_files, str):
                    license_files = [license_files]
                for name in license_files:
                    path = (manifest.parent / name).resolve(strict=True)
                    if qt_source not in path.parents:
                        raise ValueError(f"Qt license path escapes the source tree: {name}")
                    copy_required(path, licenses / path.relative_to(qt_source))
    (output / "QT_ATTRIBUTIONS.json").write_text(
        json.dumps(attributions, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    copy_required(qt_source / "sources.json", output / "QT_SOURCE_ARCHIVES.json")
    sdk_sboms = sorted(path for path in qt_prefix.glob("**/*spdx*") if path.is_file())
    if not sdk_sboms:
        raise FileNotFoundError("Qt SDK SPDX inventory is missing")
    for sbom in sdk_sboms:
        copy_required(sbom, output / "SDK_SBOM" / sbom.relative_to(qt_prefix))

    source_urls = [
        f"https://download.qt.io/official_releases/qt/{args.qt_version.rsplit('.', 1)[0]}/"
        f"{args.qt_version}/submodules/qtbase-everywhere-src-{args.qt_version}.tar.xz",
        f"https://download.qt.io/official_releases/qt/{args.qt_version.rsplit('.', 1)[0]}/"
        f"{args.qt_version}/submodules/qtsvg-everywhere-src-{args.qt_version}.tar.xz",
    ]
    (output / "README_QT_SOURCE.txt").write_text(
        f"StreamView distributes Qt {args.qt_version} under LGPLv3 terms.\n"
        "The corresponding Qt source archives are:\n"
        + "\n".join(source_urls)
        + "\nQt license text and the SDK SPDX inventory are included in this package.\n"
        "You may replace the dynamically linked Qt libraries with compatible modified versions.\n",
        encoding="utf-8",
    )

    files = []
    for path in sorted(stage.rglob("*")):
        if path.is_file():
            relative = path.relative_to(stage).as_posix()
            identifier = "SPDXRef-File-" + hashlib.sha256(relative.encode()).hexdigest()[:24]
            files.append({
                "SPDXID": identifier,
                "fileName": "./" + relative,
                "checksums": checksums(path),
                "licenseConcluded": "NOASSERTION",
                "licenseInfoInFiles": ["NOASSERTION"],
                "copyrightText": "NOASSERTION",
            })
    file_sha1 = sorted(item["checksums"][0]["checksumValue"] for item in files)
    verification_code = hashlib.sha1("".join(file_sha1).encode("ascii")).hexdigest()
    packages = [
        {
            "SPDXID": "SPDXRef-Package-StreamView",
            "name": "StreamView",
            "versionInfo": args.version,
            "downloadLocation": "https://github.com/hfliuyun/StreamView",
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "MIT",
            "filesAnalyzed": True,
            "packageVerificationCode": {"packageVerificationCodeValue": verification_code},
        },
        {
            "SPDXID": "SPDXRef-Package-Qt",
            "name": "Qt",
            "versionInfo": args.qt_version,
            "downloadLocation": "https://www.qt.io/",
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "LGPL-3.0-only",
            "filesAnalyzed": False,
        },
        {
            "SPDXID": "SPDXRef-Package-tomlplusplus",
            "name": "tomlplusplus",
            "versionInfo": "bundled",
            "downloadLocation": "https://github.com/marzer/tomlplusplus",
            "licenseConcluded": "MIT",
            "licenseDeclared": "MIT",
            "filesAnalyzed": False,
        },
        {
            "SPDXID": "SPDXRef-Package-QtSource",
            "name": "Qt source attribution inventory",
            "versionInfo": args.qt_version,
            "downloadLocation": source_urls[0],
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "NOASSERTION",
            "filesAnalyzed": False,
            "comment": "Source inventory only; not a statement of deployed component linkage.",
        },
    ]
    relationships = [
        {"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
         "relatedSpdxElement": "SPDXRef-Package-StreamView"},
        {"spdxElementId": "SPDXRef-Package-StreamView", "relationshipType": "DEPENDS_ON",
         "relatedSpdxElement": "SPDXRef-Package-Qt"},
        {"spdxElementId": "SPDXRef-Package-StreamView", "relationshipType": "DEPENDS_ON",
         "relatedSpdxElement": "SPDXRef-Package-tomlplusplus"},
        {"spdxElementId": "SPDXRef-Package-Qt", "relationshipType": "GENERATED_FROM",
         "relatedSpdxElement": "SPDXRef-Package-QtSource"},
    ]
    for file in files:
        relationships.append({
            "spdxElementId": "SPDXRef-Package-StreamView", "relationshipType": "CONTAINS",
            "relatedSpdxElement": file["SPDXID"],
        })
    for index, attribution in enumerate(attributions):
        identifier = f"SPDXRef-QtSourceAttribution-{index}"
        copyright_text = attribution.get("Copyright", "NOASSERTION")
        if isinstance(copyright_text, list):
            copyright_text = "\n".join(copyright_text) or "NOASSERTION"
        packages.append({
            "SPDXID": identifier,
            "name": attribution["Name"],
            "versionInfo": attribution.get("Version", "unspecified"),
            "downloadLocation": attribution.get("DownloadLocation") or "NOASSERTION",
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "NOASSERTION",
            "copyrightText": copyright_text,
            "filesAnalyzed": False,
            "comment": (
                "Upstream Qt source component; may be optional or replaced by a system "
                "library on this platform. Original license declaration: "
                + attribution.get("License", "unspecified")
                + ". See QT_ATTRIBUTIONS.json and SDK_SBOM/ for build provenance."
            ),
        })
        relationships.append({
            "spdxElementId": "SPDXRef-Package-QtSource", "relationshipType": "CONTAINS",
            "relatedSpdxElement": identifier,
        })
    inventory_hash = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    sbom = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"StreamView-{args.version}-{args.platform}",
        "documentNamespace": (
            f"https://github.com/hfliuyun/StreamView/spdx/"
            f"{args.version}/{args.platform}/{inventory_hash}"
        ),
        "creationInfo": {
            "created": datetime.fromtimestamp(
                args.epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "creators": ["Tool: StreamView release packaging"],
        },
        "packages": packages,
        "files": files,
        "relationships": relationships,
    }
    (output / "SBOM.json").write_text(
        json.dumps(sbom, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
