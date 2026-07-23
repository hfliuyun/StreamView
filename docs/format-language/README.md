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

The eventual reference must define primitive types, signedness, byte order, bit order, overflow behavior, arrays, enums, structures, conditionals, switches, bounded loops, pure helpers, scope, name resolution, and specification annotations. The accepted minimum subset is intentionally smaller and does not yet include expressions or control flow.

The accepted M3 type slice adds declaration-order enums and an explicit byte
order on `bits` fields. Enum declarations name unsigned integer values; a field
uses `@enum(Type)` to associate those names with its decoded value. Byte order
changes numeric interpretation only. Source bit addresses remain MSB-first and
source locations remain the bits consumed by the field.

The accepted variable-length primitive slice adds H.264-style unsigned and
signed Exp-Golomb fields with the contextual type words `ue` and `se`. These
types have no explicit width or endian argument. Their source locations cover
the complete encoded codeword rather than a fixed number of bits.

The accepted fixed-array slice adds a one-dimensional positive integer length
after a scalar field name, such as `bits<8> payload[4]`, `ue codes[3]`, or
`se deltas[3]`. The compiler expands the declaration into independently typed
and executed fields named `payload[0]` through `payload[3]`; it introduces no
array value, container node, or array-specific opcode.

## Minimum DSL 0.1 Subset

The first executable subset uses the following grammar. Whitespace and `//` or
`/* ... */` comments may appear between tokens. Identifiers use ASCII letters,
digits, and `_`, but cannot begin with a digit. Integer literals are checked
unsigned decimal or `0x` hexadecimal values. String literals use `"`, `\\`,
`\n`, `\r`, and `\t` escapes.

```text
program       := { declaration }
declaration   := { annotation } ( enum | struct | sequence | entry )
enum          := "enum" identifier "{" { enum_member } "}" [ ";" ]
enum_member   := identifier "=" integer ";"
struct        := "struct" identifier "{" { field } "}" [ ";" ]
field         := { annotation } field_type identifier [ "[" integer "]" ]
                 { annotation } ";"
field_type    := "bits" "<" integer [ "," identifier ] ">" | "ue" | "se"
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
- Enum names share the declaration namespace with structures and sequences.
  Enum member names are unique within their enum. Distinct members may name the
  same integer value; aliases accept the same decoded numeric value.
- A `bits<N>` width is an integer in `1..64`. Fields consume input in
  declaration order, most-significant bit first. With no second type argument,
  or with `big`, the resulting unsigned value is big-endian. `little` is
  accepted only for a width that is a multiple of eight, a field that begins at
  a byte boundary within its structure, and an execution whose source span
  begins at a byte boundary. It reverses byte significance without changing
  the consumed bit sequence.
- `ue` and `se` are variable-length Exp-Golomb fields. They take no width or
  endian argument and always consume the encoded codeword most-significant bit
  first. Because their width is not statically known, a later little-endian
  field is rejected unless a future language feature can prove its alignment.
- A scalar field may have one fixed array suffix `[count]`. `count` is an
  unsigned integer literal greater than zero; expressions, additional array
  dimensions, structure arrays, and runtime-sized arrays are not accepted.
  The declared base name remains the field name for duplicate-name checking.
  A structure may project at most 99,999 scalar fields after expansion, leaving
  one node for the structure within the default 100,000-node materialization
  budget. The compiler rejects a larger projection before producing executable
  typed IR.
- Fixed-width arrays contribute `width * count` bits to static alignment.
  Every element of a little-endian array must therefore have a byte-multiple
  width and the first element must begin at a structure-relative byte boundary.
  An array of `ue` or `se` fields has unknown total width, so a later
  little-endian field is rejected under the same rule as a scalar Exp-Golomb
  field.
- The only accepted progressive sequence form is
  `@index(progressive) sequence<Element> name = scan(h264_start_code);`.
  `Element` must name a declared structure.
- An `@equals(integer)` field annotation is a checked constraint and may appear
  at most once on a `bits` field. Its value must fit the field's unsigned bit
  width. An `@enum(Type)` annotation may appear at most once on a `bits` field
  and requires a declared enum type. Every declared member value must fit the
  field's bit width. Enum values are still decoded as unsigned integers; the
  enum supplies names and validation for the decoded value. `ue` and `se`
  reject both annotations.
  `@description("text")` supplies project-authored presentation text, and
  `@spec("standard", "clause")` supplies a specification reference. Fields
  inherit their structure's specification unless they provide their own. An
  array declaration applies its resolved type, annotations, metadata, and
  constraints independently to every expanded element.
- A source with lexical or static diagnostics produces no executable rule. The
  parser still returns its partial IR and all diagnostics with line/column
  ranges so an editor can report more than the first error.

`enum`, `big`, `little`, `ue`, and `se` are contextual words in the positions
shown by the grammar and remain ordinary identifiers elsewhere. Existing
scalar declarations are unchanged, and `bits<N>` remains exactly equivalent
to `bits<N, big>`; this slice deprecates no accepted 0.1 syntax.

The parser produces a source-oriented declaration model for diagnostics. The
static compiler resolves enum, structure, sequence, and entry references into a
typed program, preserves declaration order, and emits deterministic bytecode
using `begin-structure`, `read-unsigned-bits`, `read-unsigned-exp-golomb`,
`read-signed-exp-golomb`, `assert-equals`, and `end-structure` operations. Each
read opcode must match the resolved field type. The fixed-width read carries
the resolved enum and byte-order information; the Exp-Golomb types have zero
static bit width, default bit order, no enum reference, and no equality
constraint. A fixed array is expanded in source order into typed fields named
`name[0]` through `name[count - 1]`; every element emits its own read and, when
present, equality-check instruction. No alternate source-coordinate operation
is introduced. A program with any parser or compiler diagnostic has no
executable typed IR.
`svtool rule check` runs both stages. The bundled Annex B runner also compiles its rule
once when the analyzer is created and executes the resolved structure index
for every record.

The minimum VM executes a structure by reading each field through the bounded
bit reader. A successful field becomes a syntax-field node with its decoded
value and source location. For a little-endian field, the VM reverses
the significance of complete source bytes after reading them; it never changes
the bit reader position or the source mapping. Reaching a little-endian field
at a non-byte-aligned source address is an invalid typed execution and does not
consume that field. An enum field retains its numeric value and type metadata.
A value not declared by that enum retains the
field node, marks the structure invalid, and reports an `invalid-syntax`
diagnostic at that field. A truncated or failed read retains earlier fields and
marks the structure invalid with a source diagnostic. An `@equals` mismatch
retains the field, then marks the structure invalid with an invalid-syntax
diagnostic. Each expanded array element becomes a separate syntax-field node
whose source location covers only that element. A failure keeps all earlier
complete elements, creates no node for an incomplete element, and uses the
expanded path such as `Header.values[2]` in its diagnostic. The minimum
executor requires the logical range to map to one contiguous direct source
span; mapped multi-span transformations are reserved
for the later mapped-transformation runtime. The executor retains the field
type, description, and specification reference on the analysis-node snapshot;
presentation derives field width from that node's logical range.

For an Exp-Golomb codeword, let `leadingZeroBits` be the number of zero bits
before the marker bit and let `suffix` be the following unsigned value of the
same width. `ue` returns
`codeNum = (2^leadingZeroBits - 1) + suffix` as an unsigned 64-bit value. `se`
maps that code number to `0` when it is zero, `+(codeNum / 2 + 1)` when it is
odd, and `-(codeNum / 2)` when it is even, and publishes a signed 64-bit value.
Thus the first signed values are `0, +1, -1, +2, -2`.

At most 63 leading zero bits are representable. The longest valid codeword is
127 bits and the largest `ue` value is `2^64 - 2`. Encountering a 64th leading
zero reports `invalid-syntax`. The prefix, marker, and suffix are one
transactional field read: truncation, source failure, or overflow seeks the
reader back to the field start, creates no partial field node, retains earlier
complete fields, and attaches a field-path diagnostic anchored at the failed
field. A successful node's logical range and source span cover the entire
codeword.

Each `ue` or `se` field is one VM instruction even though that instruction may
read up to 127 bits. Its internal component reads create no additional nodes
and add no cancellation point; cancellation remains checked at the documented
instruction interval. The hard 127-bit bound keeps the work within one
instruction bounded.

The built-in `h264_start_code` scanner reads the source through a 64 KiB random
access window and publishes `H264StartCodeRecord` values in batches bounded by
both record count and inspected source positions. The default work budget is
64 KiB of source positions per call, and the monotonic scan cursor provides UI
progress. Each record contains the three- or four-byte start-code span and the
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

Valid enum and endian example:

```cpp
enum PacketKind {
    payload = 1;
    control = 2;
}

struct PacketHeader {
    bits<16, little> payload_size;
    bits<8> kind @enum(PacketKind);
}

entry PacketHeader;
```

Valid Exp-Golomb example:

```cpp
@spec("ITU-T H.264", "7.3.3")
struct SliceHeaderPrefix {
    ue first_mb_in_slice;
    ue slice_type;
    se slice_qp_delta @description("Signed QP delta.");
}

entry SliceHeaderPrefix;
```

Valid fixed-array example:

```cpp
enum SampleKind {
    luma = 1;
    chroma = 2;
}

struct Samples {
    bits<2> kinds[4] @enum(SampleKind);
    bits<16, little> values[2] @description("Little-endian samples.");
    ue run_lengths[3];
}

entry Samples;
```

Invalid minimum examples include `bits<0> flag;`, `bits<65> flag;`,
`bits<12, little> value;`, a little-endian field after an unaligned field,
`ue value @equals(0);`, `se value @enum(Type);`, a little-endian field after a
variable-length field, `bits<1> flags[0];`, `bits<1> flags[];`, an expression or
second dimension in an array length, a structure projection above 99,999
fields, a truncated array element, a truncated Exp-Golomb codeword, 64 leading
zero bits, `@enum(Missing)`, an enum member value that does not fit its field,
duplicate enum member names, a sequence without `@index(progressive)`,
`scan(other_scanner)`, two declarations with the same name, or a program with
no `entry`. Enum and field parsing recovers at the next member/field semicolon
or closing brace and preserves all source ranges and diagnostics.

## Source And Logical Coordinates

The unchanged media source uses absolute source coordinates. A logical view has its own logical coordinates and an ordered mapping back through all parent views to absolute source spans. A syntax field may map to multiple disjoint source spans.

Byte order is a value-interpretation rule, not a coordinate rule. Explicit
`little` therefore leaves the logical range, absolute source spans, selection,
and diagnostics identical to the default big-endian read.

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

Rules receive bounded, read-only access to the current media source. The runtime
limits execution steps, input and output ranges, recursion depth, node count,
memory, and wall-clock work between cancellation points. Rules cannot access
arbitrary files, networks, processes, environment variables, host pointers, or
native plug-ins.

The current VM applies these defaults to one structure materialization:

- at most 1,000,000 bytecode instructions;
- call depth 64 and mapped-view depth 64;
- analysis-node depth 256, counting the root as depth 1;
- at most 100,000 newly materialized nodes; and
- a cancellation poll before the first instruction and at least every 1,024
  executed instructions thereafter.

Enum membership validation and byte-order conversion are part of the existing
field-read operation. They do not add source reads or analysis nodes, and they
use the same instruction-budget and cancellation boundaries.

Array syntax does not reserve a separate runtime budget. Every expanded
element consumes one materialized node and one read instruction; `@equals`
adds one assertion instruction per element. Truncation, a failed constraint,
an instruction limit, or a node limit can therefore stop between elements
while preserving the elements completed before the failure. The static
99,999-field projection limit ensures one default structure materialization
cannot require more than the documented 100,000 nodes.

All limits must be greater than zero. The host may lower them for a particular
execution but a rule cannot raise or inspect them. The accepted minimum subset
does not yet contain nested calls or views; those operations must consume the
already reserved depth budgets when introduced. An instruction, node-count, or
node-depth breach reports `resource-limit`, retains nodes completed before the
breach, and marks the active structure invalid. Cancellation reports
`cancelled`, retains completed nodes, and marks the active structure (or its
parent when cancellation precedes `begin-structure`) cancelled. Invalid or
malformed typed bytecode is rejected as an invalid definition rather than
executed heuristically.

Exact input/output, memory, and wall-clock defaults remain provisional and must
be documented before the features that consume them become stable.

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
