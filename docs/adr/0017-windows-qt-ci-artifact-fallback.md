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
- Keep the Windows fallback explicit in the workflow matrix and do not use it
  for release artifacts.
- Revert the Windows matrix entry to 6.11.1 as soon as the following public
  metadata path returns successfully and contains `win64_msvc2022_64`:
  `https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6111/qt6_6111/Updates.xml`

## Consequences

The phase-0 hosted build gate can run on all three platforms immediately, but
the Windows job does not yet exercise the exact preferred Qt patch version.
The fallback must be removed before release packaging is treated as a fully
uniform Qt 6.11.1 matrix.
