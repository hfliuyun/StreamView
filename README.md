# StreamView

StreamView is a read-only desktop analyzer for inspecting media containers and
codec syntax down to specification-defined bits. The first milestone targets
H.264 Annex B, AAC ADTS, and non-fragmented MP4/MOV on Windows, macOS, and
Linux.

- [Product requirements](docs/product-requirements.md)
- [Implementation plan](docs/implementation-plan.md)
- [Format language design](docs/format-language/README.md)
- [中文说明](README.zh-CN.md)

## Development status

The project is under active construction. The current implementation status,
verification evidence, and next resumable action are recorded in the
[implementation plan](docs/implementation-plan.md).

## Build prerequisites

- CMake 3.28 or newer
- Ninja
- A C++20 compiler
- Qt 6.11.x with Core, Gui, Widgets, Concurrent, Sql, and Test

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

## Internal CLI

The development build includes `svtool` for rule validation and the current
Annex B analysis slice:

```sh
svtool --version
svtool rule check <rule.svfmt>
svtool analyze <source>
```

Exit code `0` means success, `1` means the rule or analysis produced
diagnostics/partial results, and `2` means invalid usage or an input could not
be opened. The CLI is an internal development tool, not the deferred public CLI
contract.

## Release packages

Stable version tags matching the CMake project version build Windows x64 ZIP,
macOS ARM64 `.app.zip`, and Linux x86_64 AppImage packages and upload them to
GitHub Release. The Linux compatibility floor is glibc 2.35 (Ubuntu 22.04).
Packages include license material and a deployment SBOM; Release assets also
include platform SPDX inventories and archive checksums. Signing and macOS
notarization remain deferred. There is no alpha/beta/rc prerequisite.

Release implementation and current verification status are tracked in the
[packaging contract](docs/p7f-h-release-task.md) and implementation plan.

StreamView is licensed under the [MIT License](LICENSE). Qt and other bundled
dependencies retain their respective licenses.
