# P7f-P7h: Tag Release Packaging

The release workflow accepts stable `vX.Y.Z` tags only and checks the tag
against the CMake project version. The GUI and internal CLI share that
version. Ordinary CI uploads no build or test artifacts.
A manual dispatch runs the same packaging checks without creating a tag or
publishing a Release; its temporary packages also have one-day retention.

All packages use Qt 6.11.1. Windows must install the MSVC 2022 x64 archive;
there is no Qt 6.10 fallback. Windows uses CPack ZIP and Qt's deployment
hook, followed by a smoke test of the extracted archive. CPack archives the
verified deployment tree, without reinstalling project targets. macOS installs
the deployed bundle, names it `StreamView.app`, includes compliance material
in the sibling `StreamView-materials/` directory, and uses `ditto` to make `.app.zip`.
The macOS product remains unsigned/not notarized for public distribution.
The build and deployment tools may supply local ad hoc signatures.
The workflow verifies these signatures before and after archiving. Materials
remain outside the bundle so they do not invalidate its resource seal.

Linux builds on Ubuntu 22.04 to preserve the glibc 2.35 floor. It uses
the upstream linuxdeployqt continuous AppImage with the pinned SHA256
`974a87457ed26241b793bed7841978fcdf84158d13220e53833a06515f173b0b`.
A changed upstream asset fails the digest check. Its desktop entry, icon,
GUI, CLI, Qt plugins and non-Qt dependencies are collected into AppDir.
The workflow checks both the extracted package and its AppImage launcher.
AppImage assembly uses the appimagetool embedded in the verified linuxdeployqt
image, without deploying or stripping the inventoried binaries a second time.
Qt's glibc 2.39 installer note applies to Linux Arm; this x86_64 release uses
the documented Qt 6.10+ glibc 2.34 baseline and runs its package on glibc 2.35.

Qt source archives are fetched from versioned official release paths and
verified against the upstream SHA256 files. Original Qt license texts,
source dependency attributions, source archive URLs/digests, and SDK SPDX
inventories are preserved. `SBOM.json` records each staged file using a
path-based unique ID and checksum. It separately labels the full Qt source
attribution inventory; optional source components are not proof of linkage
on every platform. Unknown file licenses remain `NOASSERTION`, rather than
being invented. The SDK inventories and Linux deployed copyright notices
are the dependency audit evidence.

Material generation happens after runtime deployment and before package
assembly. Material files themselves are excluded from their own deployment
inventory. Each bundle contains `LICENSE`, `LICENSES/`, `SBOM.json`,
`SDK_SBOM/`, `QT_ATTRIBUTIONS.json`, `QT_SOURCE_ARCHIVES.json`, and
`README_QT_SOURCE.txt`. Each platform's SPDX JSON is also a separate Release
asset. `SHA256SUMS.txt` covers the three final archives.

The package smoke test clears SDK library/plugin environment variables,
starts the packaged GUI, and verifies the expected version, SQLite driver,
and three embedded rule packages. Windows and Linux also run the packaged
CLI on a minimal Annex B file. Required license and SBOM files must exist.
Extracted files must match the SBOM paths and SHA256 checksums, with no
unrecorded deployment files. All three package jobs must succeed before publication. Only the publish
job has repository write permission. Intermediate release artifacts have
one-day retention; final assets reside on GitHub Release.

P7h remains an independent review gate. Local macOS packaging and static
workflow checks do not establish Windows/Linux delivery or minimum-system
compatibility. No release tag is created merely to obtain CI evidence.
P7i-P7l alpha/beta/rc/GA sequencing remains deferred.
