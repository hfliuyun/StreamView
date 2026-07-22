# StreamView Format Definition Language

Status: design draft with the minimum 0.1 subset accepted below. Features outside
that subset remain provisional until separately accepted.

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

The eventual reference must define primitive types, signedness, byte order, bit order, overflow behavior, arrays, enums, structures, conditionals, switches, bounded loops, pure helpers, scope, name resolution, and specification annotations. The accepted minimum subset is intentionally smaller and does not yet include expressions, arrays, or control flow.

## Minimum DSL 0.1 Subset

The first executable subset uses the following grammar. Whitespace and `//` or
`/* ... */` comments may appear between tokens. Identifiers use ASCII letters,
digits, and `_`, but cannot begin with a digit. Integer literals are checked
unsigned decimal or `0x` hexadecimal values. String literals use `"`, `\\`,
`\n`, `\r`, and `\t` escapes.

```text
program       := { declaration }
declaration   := { annotation } ( struct | sequence | entry )
struct        := "struct" identifier "{" { field } "}" [ ";" ]
field         := { annotation } "bits" "<" integer ">" identifier
                 { annotation } ";"
sequence      := "sequence" "<" identifier ">" identifier "="
                 "scan" "(" identifier ")" ";"
entry         := "entry" identifier ";"
annotation    := "@" identifier [ "(" [ value { "," value } ] ")" ]
value         := integer | string | identifier
```

The static rules for this subset are:

- A program has exactly one `entry`; its target names a declared structure or
  sequence.
- Structure and sequence names are unique across the program. Field names are
  unique within a structure, and a structure contains at least one field.
- A `bits<N>` width is an integer in `1..64`. Fields are unsigned and consume
  input in declaration order, most-significant bit first.
- The only accepted progressive sequence form is
  `@index(progressive) sequence<Element> name = scan(h264_start_code);`.
  `Element` must name a declared structure.
- An `@equals(integer)` field annotation is a checked constraint. Other
  annotations are retained as metadata; `@spec("standard", "clause")` is the
  conventional specification reference.
- A source with lexical or static diagnostics produces no executable rule. The
  parser still returns its partial IR and all diagnostics with line/column
  ranges so an editor can report more than the first error.

The minimum runtime executes a structure by reading each field through the
bounded bit reader. A successful field becomes a syntax-field node with its
decoded unsigned value and source location. A truncated or failed read retains
earlier fields and marks the structure invalid with a source diagnostic. An
`@equals` mismatch retains the field, then marks the structure invalid with an
invalid-syntax diagnostic. The minimum executor requires the logical range to
map to one contiguous direct source span; mapped multi-span transformations are
reserved for the later mapped-transformation runtime.

The built-in `h264_start_code` scanner reads the source through a 64 KiB random
access window and publishes `H264StartCodeRecord` values in caller-sized
batches. Each record contains the three- or four-byte start-code span and the
following NAL-unit span (an empty final unit has no payload span). Prefixes may
cross a window boundary. Cancellation is checked at least every 1,024 inspected
source positions; already published records remain valid and the batch reports
`cancelled`. The NAL offset and zero length remain valid for an empty unit even
though its optional payload span is absent. A source whose byte size cannot be
represented by the 64-bit source-bit coordinate model is rejected before the
scanner reads it.

The built-in H.264 Annex B candidate detector inspects at most the first 64 KiB
of an already loaded source prefix. It does not use the file name or extension.
Each detected three- or four-byte start code contributes source-located
evidence; when the following byte is available, the evidence also records its
NAL-unit-header span, `forbidden_zero_bit` result, and `nal_unit_type`.

A complete start code without a syntactically plausible header is `weak`
evidence. One header with `forbidden_zero_bit == 0` and `nal_unit_type` in
`1..23` raises the candidate confidence to `probable`; two or more such headers
raise it to `strong`. The result reports the exact number of inspected bytes and
whether that prefix covered the complete source. No candidate in a partial
64 KiB probe means only that no Annex B signature was found within the probe;
it does not reject the source or make the eventual rule selection. Format
detection remains a recommendation, and explicit rule selection may override
it.

### Bundled Annex B Profile

The bundled minimum H.264 rule uses the grammar above; the tree projection in
this section is profile/runtime behavior rather than additional DSL syntax. For
each scanner record, the Annex B runner publishes a `nal_unit[index]` region
whose source location covers the start code and any non-empty NAL payload. Its
`start_code` child covers only the three- or four-byte prefix. A
`NalUnitHeader` child consumes exactly the first eight payload bits and exposes
`forbidden_zero_bit`, `nal_ref_idc`, and `nal_unit_type`; payload bits after the
header remain uninterpreted by this minimum profile.

An empty final NAL still publishes its NAL region and start-code child. It also
publishes an invalid, zero-field `NalUnitHeader`; the containing NAL's
`truncated-source` summary diagnostic is anchored to the known NAL region. An
`@equals(0)` mismatch retains `forbidden_zero_bit`, marks the header and
containing NAL invalid, and does not prevent the overall scan from completing.
Header read failures retain published nodes, mark the root invalid, and report
`source-error`. Cancellation retains completed NAL regions and marks the root
cancelled.

Valid minimum example:

```cpp
@spec("ITU-T H.264", "7.3.1")
struct NalUnitHeader {
    bits<1> forbidden_zero_bit @equals(0);
    bits<2> nal_ref_idc;
    bits<5> nal_unit_type;
}

@index(progressive)
sequence<NalUnitHeader> nal_units = scan(h264_start_code);
entry nal_units;
```

Invalid minimum examples include `bits<0> flag;`, `bits<65> flag;`, a sequence
without `@index(progressive)`, `scan(other_scanner)`, two declarations with the
same name, or a program with no `entry`.

## Source And Logical Coordinates

The unchanged media source uses absolute source coordinates. A logical view has its own logical coordinates and an ordered mapping back through all parent views to absolute source spans. A syntax field may map to multiple disjoint source spans.

Selecting a syntax field highlights every mapped source span. Selecting a
source bit resolves to the most specific materialized node while preserving its
analysis-tree ancestry. Resolution uses the deterministic depth, source
coverage, and stable-node-ID ordering defined by the
[analysis model](../analysis-model.md#source-bit-resolution).

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
