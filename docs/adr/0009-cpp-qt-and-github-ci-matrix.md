# C++/Qt Core With A GitHub Actions Matrix

StreamView will use a C++20 analysis core, Qt 6 Widgets for the desktop UI, and CMake for builds. Platform-specific APIs stay behind a small boundary; the core uses explicit fixed-width integer and bit-reader types and shares the same conformance vectors on every platform. GitHub Actions will build and test the primary Windows, macOS, and Ubuntu targets on every change, then create platform-specific deployment artifacts from the same tagged source revision using Qt's CMake deployment support and platform deployment tools.

**Consequences**

- Cross-platform behavior is a tested contract, not an assumption based on one successful local build.
- The Linux artifact must be built against a deliberately chosen baseline because Linux distribution compatibility is not uniform.
- Release packaging, code signing, and notarization remain separate release concerns and are not implied by a passing CI build.
