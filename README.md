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

StreamView is licensed under the [MIT License](LICENSE). Qt and other bundled
dependencies retain their respective licenses.
