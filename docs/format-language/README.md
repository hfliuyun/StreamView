# StreamView Format Definition Language

Status: design draft. Examples illustrate agreed semantics; exact grammar and spelling remain provisional until separately accepted.

The StreamView Format Definition Language describes container and codec syntax without executing unrestricted native or scripting-language code. Built-in and user-installed rules use the same language and runtime.

## Documentation Contract

Before a language feature is considered stable, this reference must document:

- its complete syntax and static rules;
- its runtime semantics and source-coordinate behavior;
- all diagnostics and recovery behavior;
- resource limits and cancellation points;
- compatibility and deprecation rules;
- at least one valid example and relevant invalid examples.

Behavior implemented only in C++ or demonstrated only by an example is not part of the public language contract.

## Design Principles

- C-style declarations and control flow are familiar to C and C++ authors.
- Field declarations are read-only and consume input in a deterministic order.
- Every encoded syntax field maps exactly to one or more source spans.
- Computed fields never pretend to have source locations.
- All input access and work are bounded, checked, and cancellable.
- The language has no host, network, process, pointer, or native-library access.
- Composition is preferred; C++ object lifetime, inheritance, templates, and mutation are outside the language.

## Field Declarations

A field declaration consumes bits from the current view and creates a syntax field. A computed field derives a named value without consuming bits.

```cpp
@spec("ITU-T H.264", "7.3.1")
struct NalUnitHeader {
    bits<1> forbidden_zero_bit @equals(0);
    bits<2> nal_ref_idc;
    bits<5> nal_unit_type;

    computed<bool> is_vcl =
        nal_unit_type >= 1 && nal_unit_type <= 5;
}
```

The eventual reference must define primitive types, signedness, byte order, bit order, overflow behavior, arrays, enums, structures, conditionals, switches, bounded loops, pure helpers, scope, name resolution, and specification annotations.

## Source And Logical Coordinates

The unchanged media source uses absolute source coordinates. A logical view has its own logical coordinates and an ordered mapping back through all parent views to absolute source spans. A syntax field may map to multiple disjoint source spans.

Selecting a syntax field highlights every mapped source span. Selecting source bits resolves to the most specific materialized field while preserving its analysis-tree ancestry.

## Mapped Transformations

A mapped transformation may forward, skip, or slice input while preserving the origin of every forwarded bit. Excluded source spans remain visible and carry a named structural role.

```cpp
view rbsp from ebsp {
    while (!input.eof()) {
        if (next_is_emulation_prevention_byte()) {
            skip bits<8> as emulation_prevention_byte;
        } else {
            forward bits<8>;
        }
    }
}
```

Rules cannot manufacture logical bits and expose them as source-backed fields. Values with no exact source mapping must be represented as computed fields.

## Lazy Regions And Progressive Indexes

Rules explicitly declare safe boundaries for content that can be materialized later. A progressive index publishes structures in batches during a cancellable, resumable scan.

```cpp
struct Mp4Box {
    be_u32 size;
    fourcc type;

    @lazy(size - 8)
    bytes payload;
}

@index(progressive)
sequence<NalUnit> nal_units = scan(h264_start_code);
```

A lazy boundary is registered only after its size and enclosing limit have been checked. The analysis model distinguishes lazy, indexing, cancelled, unsupported, invalid, and completely materialized states.

## Sandbox And Resource Limits

Rules receive bounded, read-only access to the current media source. The runtime limits execution steps, input and output ranges, recursion depth, node count, memory, and wall-clock work between cancellation points. Rules cannot access arbitrary files, networks, processes, environment variables, host pointers, or native plug-ins.

The exact default limits, configuration policy, and diagnostics remain to be designed and must be documented here before the language is implemented as stable.

## Rule Packages

A rule package is versioned and declares its format identity, engine compatibility, applicability metadata, and dependencies. Format detection recommends candidates; the user may always override the selection. An analysis session records the exact selected rule and version.

Application, language, and rule-package versions are independent. Package manifests declare an exact language contract and an engine compatibility range. During the `0.x` language phase, documented breaking changes are permitted; after language `1.0`, incompatible changes require a new major language version. An incompatible package is rejected with a diagnostic rather than interpreted heuristically.

First-release packages are self-contained and cannot resolve dependencies from the network at analysis time. The package manifest syntax, dependency rules, and trust policy remain to be designed.

Official rules are bundled with a particular application release. Additional packages may be installed only from a local file or directory in the first release; there is no online marketplace, automatic download, or automatic update. Installation presents package identity, version, format coverage, author metadata, content hash, and compatibility range. Saved sessions pin the exact selected package version, and installed rules receive no additional permissions based on claimed author or trust status.

### Provisional Package Layout

During development, a package is a directory with a TOML manifest, C-style format definitions, localized documentation, and distributable tests:

```text
org.streamview.h264/
├── rule.toml
├── src/
├── docs/
│   ├── en/
│   └── zh-CN/
└── tests/
```

For local installation, the directory may be encoded as a deterministic ZIP container with a dedicated extension that remains to be selected. The manifest identifies the package, author, license, package version, language contract, engine compatibility range, entry points, declared format/profile/depth coverage, detection metadata, and localized documentation.

Packaged rules contain no native executable code or symbolic links. Installers reject absolute paths, parent traversal, duplicate or non-normalized paths, and entries escaping the package root. Installed content is retained read-only by content hash. Exact manifest keys, archive canonicalization, extension, and size limits remain provisional.
