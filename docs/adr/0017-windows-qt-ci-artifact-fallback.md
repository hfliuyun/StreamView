# Windows Qt CI Artifact Fallback

Status: Accepted
Date: 2026-07-19

## Context

The project baseline targets Qt 6.11.x and the CI matrix was initially pinned
to Qt 6.11.1. The official Qt online repository currently returns HTTP 404 for
the Windows metadata path used by `aqtinstall`:

`windows_x86/desktop/qt6_6111/qt6_6111/Updates.xml`

The same 6.11.1 metadata is available for the Linux runner, while the Windows
repository exposes Qt 6.10.1 with the required MSVC 2022 64-bit package. This
is an upstream artifact publication gap, not a source or compiler failure.

## Decision

- Keep Qt 6.11.x as the product and development baseline.
- Keep Ubuntu and macOS CI on Qt 6.11.1.
- Use Qt 6.10.1 only for the Windows CI job until the official Windows Qt
  6.11.1 metadata and package are published.
- Keep the CMake minimum Qt version at 6.11 by default. The Windows CI job may
  explicitly set `STREAMVIEW_MINIMUM_QT_VERSION=6.10` for this fallback only.
- Keep the Windows fallback explicit in the workflow matrix and do not use it
  for release artifacts.
- Revert the Windows matrix entry to 6.11.1 as soon as the following public
  metadata path returns successfully and contains `win64_msvc2022_64`:
  `https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6111/qt6_6111/Updates.xml`

## Consequences

The phase-0 hosted build gate can run on all three platforms immediately, but
the Windows job does not yet exercise the exact preferred Qt patch version.
The fallback must be removed before release packaging is treated as a fully
uniform Qt 6.11.1 matrix. Release builds must not lower
`STREAMVIEW_MINIMUM_QT_VERSION`.

## October 13, 2026 Restoration

The MSVC 2022 x64 repository is now present at
`windows_x86/desktop/qt6_6111/qt6_6111_msvc2022_64/`.
The old unqualified `qt6_6111/Updates.xml` subdirectory still returns 404:
the repository layout changed, so availability must be checked at the
architecture-specific directory.

The published aqtinstall 3.3.0 does not resolve this Windows layout. CI and
release packaging pin upstream aqtinstall commit
`076e1659807d0b362a3ed684d54c2e9c775eb9c7` with Python 3.12 and py7zr 1.1.4.
A local Python 3.11 dry-run of that exact installer successfully resolved
qtbase, qtsvg, qttools and the remaining desktop archives for Qt 6.11.1
`win64_msvc2022_64`. This proves repository resolution, not a Windows build.

The ordinary matrix restores Qt 6.11.1 and removes the minimum-version
override. Hosted compilation and tests must confirm the restoration before
the platform gate closes; no fallback is permitted in release packages.
