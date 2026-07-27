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

The eventual reference must define primitive types, signedness, byte order, bit order, overflow behavior, arrays, enums, structures, conditionals, switches, bounded loops, pure helpers, scope, name resolution, and specification annotations. The accepted minimum subset remains intentionally bounded: expressions are accepted only in pure-function return values and computed fields, and control flow remains limited to the conditional, switch, and bounded-repeat forms described below.

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

The accepted conditional slice adds nested
`if (previous_field == integer) { ... } else { ... }` blocks. `else` is
optional. The comparison is a restricted presence decision over a previous
scalar fixed-width value, not a general expression.

The accepted switch slice adds nested
`switch (previous_field) { case integer: { ... } default: { ... } }` blocks.
Each arm has its own braced body, `default` is optional, and selection uses the
same restricted equality guards as the conditional slice.

The accepted bounded-repeat slice adds nested
`repeat (previous_count, maximum) { ... }` blocks. A decoded unsigned count
selects zero through `maximum` projected iterations. The positive literal
`maximum` bounds both the compiled field projection and the accepted runtime
count; this is not a general loop or count expression.

The accepted computed-value slice adds top-level expression-bodied `pure`
scalar functions and structure-local `computed<bool>` or `computed<u64>`
fields. Pure calls are statically inlined into a bounded typed expression;
computed fields consume no source bits and never receive a source location.
This adds no runtime call stack, mutable state, or host access.

## Minimum DSL 0.1 Subset

The first executable subset uses the following grammar. Whitespace and `//` or
`/* ... */` comments may appear between tokens. Identifiers use ASCII letters,
digits, and `_`, but cannot begin with a digit. Integer literals are checked
unsigned decimal or `0x` hexadecimal values. String literals use `"`, `\\`,
`\n`, `\r`, and `\t` escapes.

```text
program       := { declaration }
declaration   := pure_function | { annotation } ( enum | struct | sequence | entry )
pure_function := "pure" scalar_type identifier "("
                 [ parameter { "," parameter } ] ")"
                 "{" "return" expression ";" "}"
parameter     := scalar_type identifier
enum          := "enum" identifier "{" { enum_member } "}" [ ";" ]
enum_member   := identifier "=" integer ";"
struct        := "struct" identifier "{" { struct_item } "}" [ ";" ]
struct_item   := field | computed | conditional | switch | repeat
field         := { annotation } field_type identifier [ "[" integer "]" ]
                 { annotation } ";"
field_type    := "bits" "<" integer [ "," identifier ] ">" | "ue" | "se"
computed      := { annotation } "computed" "<" scalar_type ">" identifier
                 "=" expression { annotation } ";"
scalar_type   := "bool" | "u64"
conditional   := "if" "(" ( identifier "==" integer | identifier ) ")"
                 "{" { struct_item } "}"
                 [ "else" "{" { struct_item } "}" ]
switch        := "switch" "(" identifier ")" "{"
                 switch_case { switch_case } [ switch_default ] "}"
switch_case   := "case" integer ":" "{" { struct_item } "}"
switch_default := "default" ":" "{" { struct_item } "}"
repeat        := "repeat" "(" identifier "," integer ")"
                 "{" { struct_item } "}"
sequence      := "sequence" "<" identifier ">" identifier "="
                 "scan" "(" identifier ")" ";"
entry         := "entry" identifier ";"
annotation    := "@" identifier [ "(" [ value { "," value } ] ")" ]
value         := integer | string | identifier
expression    := logical_or
logical_or    := logical_and { "||" logical_and }
logical_and   := equality { "&&" equality }
equality      := relational { ( "==" | "!=" ) relational }
relational    := additive { ( "<" | "<=" | ">" | ">=" ) additive }
additive      := multiplicative { ( "+" | "-" ) multiplicative }
multiplicative := unary { ( "*" | "/" | "%" ) unary }
unary         := "!" unary | primary
primary       := integer | "true" | "false" | identifier
                 | identifier "(" [ expression { "," expression } ] ")"
                 | "(" expression ")"
```

The static rules for this subset are:

- A program has exactly one `entry`; its target names a declared structure or
  sequence.
- Structure, sequence, enum, and pure-function names share one top-level
  declaration namespace. Field names are unique within a structure across
  syntax and computed declarations, and a structure contains at least one
  syntax or computed field.
- Enum member names are unique within their enum. Distinct members may name the
  same integer value; aliases accept the same decoded numeric value.
- A pure function declares a `bool` or `u64` return type, at most 16 uniquely
  named `bool` or `u64` parameters, and exactly one `return` expression. It may
  reference only its parameters and pure functions declared earlier. Function
  overloads, forward calls, direct or indirect recursion, and annotations on a
  pure function are rejected. Every body is type-checked and checked against
  the expression limits even when the function is unused.
- A `bits<N>` width is an integer in `1..64`. Fields consume input in
  declaration order, most-significant bit first. With no second type argument,
  or with `big`, the resulting unsigned value is big-endian. `little` is
  accepted only for a width that is a multiple of eight, a field that begins at
  a byte boundary within its structure, and an execution whose absolute logical
  field start and first resolved source bit both begin at byte boundaries.
  Later mapping-segment boundaries need not be source-byte-aligned. `little`
  reverses logical-byte significance without changing the consumed bit sequence.
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
- An equality-conditional controller must name an earlier scalar `bits`, enum,
  or `computed<u64>` field guaranteed to have been materialized on every path
  reaching that condition. Arrays, `ue`, `se`, `computed<bool>`, future or
  unknown fields, and a branch-local field used outside its guaranteeing branch
  are rejected. The integer must fit a source field's unsigned bit width;
  `computed<u64>` accepts the complete `u64` literal range. The shorthand
  `if (flag)` accepts only an earlier guaranteed `computed<bool>` and means
  equality with `true`. Conditions may nest, and both branches are statically
  checked even though only one executes. General condition expressions,
  Boolean combinations, `else if`, and other comparison forms are not accepted.
- Field names remain unique across all branches of a structure. Every possible
  branch field counts toward the 99,999-field projection limit. Static
  alignment is tracked independently through both branches; the conditional
  exit retains a known offset only when both paths end at the same known
  offset, with an omitted `else` treated as an empty path.
- A switch controller follows the equality conditional's declaration-order and
  path-availability rules and accepts scalar `bits`, enum, or `computed<u64>`,
  but not `computed<bool>`. A switch contains at least one `case`. Each case
  accepts exactly one distinct unsigned integer literal that fits a source
  controller's width; `computed<u64>` accepts the complete `u64` range.
  `default` is optional, may appear at most once, and must be the final arm.
  Fallthrough, `break`, multiple labels for one body, ranges, enum member names,
  and general expressions are not accepted. Switches and conditionals may nest
  in either order.
- Every switch arm is statically checked, and field names remain unique across
  every arm and surrounding branch. All arm fields count toward the 99,999-
  field projection limit. Each arm starts with the same incoming static
  offset; the switch exit retains a known offset only when all paths end at the
  same known offset. When `default` is omitted, an unmatched empty path also
  participates in that merge.
- A repeat controller must name an earlier scalar unsigned `bits`, enum, `ue`,
  or `computed<u64>` field guaranteed to have been materialized on every path
  reaching the statement. Arrays, `se`, `computed<bool>`, unknown or future
  fields, and branch-local fields used outside their guaranteeing branch are
  rejected. The `maximum` is a positive unsigned integer literal and must fit a
  fixed-width source controller; a computed controller accepts the complete
  `u64` range.
  Count expressions, sentinel or EOF termination, and `break` are not accepted.
  A repeat body must contain at least one syntax or computed field and may
  contain either field form, conditionals, switches, and nested repeats.
- The compiler validates and projects a repeat body exactly `maximum` times.
  Source declaration names remain unique across the complete structure. A body
  declaration is visible to later items in the same iteration, but repeat-local
  declarations are unavailable in another iteration or after the repeat.
  Materialized names append enclosing repeat indexes from outermost to
  innermost, followed by an optional fixed-array index: `value[i]`,
  `value[outer][inner]`, and `value[i][j]`. Every projected field counts toward
  the 99,999-field limit.
- Static alignment is checked separately through every projected iteration, so
  a fixed alignment error inside a repeat is rejected. Because the runtime may
  select any count from zero through `maximum`, the structure-relative offset
  after a repeat is considered unknown. A later little-endian field is rejected
  even when a future expression analysis might be able to prove it aligned.
- A computed field declares `computed<bool>` or `computed<u64>` and one
  expression. It may reference earlier scalar unsigned `bits`, enum, `ue`, or
  computed fields guaranteed on every path reaching the declaration. Arrays,
  `se`, unknown or future fields, and unavailable branch-local values are
  rejected. A computed field consumes zero bits, leaves static alignment
  unchanged, inherits enclosing guards, counts toward the 99,999-field
  projection limit, and is visible to later declarations under the same scope
  rules as a syntax field. Repeat projection appends the same indexes to its
  materialized name.
- Expressions accept unsigned integer and Boolean literals, identifiers, calls
  to declared pure functions, parentheses, unary `!`, checked `*`, `/`, `%`, `+`,
  and `-`, same-type `==` and `!=`, unsigned ordering, and short-circuit Boolean
  `&&` and `||`, with the precedence shown by the grammar. There are no implicit
  conversions: arithmetic and ordering require `u64`, logical operators require
  `bool`, equality operands have the same type, and function arguments exactly
  match their parameters. Unsigned overflow or underflow, division by zero, and
  remainder by zero are runtime `invalid-syntax` failures at the computed field
  path. Enum fields contribute their decoded `u64`; enum member names are not
  expression values.
- Every written pure-function body or computed-field expression, and every fully
  inlined computed expression, has depth at most 64 and at most 256 nodes.
  Expanding one body or computed expression may
  perform at most 4,096 shared work steps across calls, arguments, and parameter
  substitutions, including arguments for parameters the callee does not use.
  Pure calls are expanded statically and introduce no runtime call, recursion,
  source access, host access, time, randomness, or mutable state.
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
  constraints independently to every expanded element. A computed field accepts
  `@description` and `@spec`, before the declaration or after its expression,
  but rejects `@equals`, `@enum`, and an array suffix.
- A source with lexical or static diagnostics produces no executable rule. The
  parser still returns its partial IR and all diagnostics with line/column
  ranges so an editor can report more than the first error.

`enum`, `big`, `little`, `ue`, `se`, `pure`, `return`, `bool`, `u64`,
`computed`, `true`, `false`, `if`, `else`, `switch`, `case`, `default`, and
`repeat` are contextual words in the positions shown by the grammar and remain
ordinary identifiers elsewhere.
Existing scalar declarations are unchanged, and `bits<N>` remains exactly
equivalent to `bits<N, big>`; this slice deprecates no accepted 0.1 syntax.

The parser produces a source-oriented declaration model for diagnostics. The
static compiler resolves pure functions, enums, structures, sequences, and entry
references into a typed program, preserves declaration order, and emits
deterministic bytecode using `begin-structure`, `read-unsigned-bits`,
`read-unsigned-exp-golomb`, `read-signed-exp-golomb`, `evaluate-computed`,
`assert-equals`, `assert-repeat-count`, and `end-structure` operations. Each read
opcode must match the resolved field type.
The fixed-width read carries the resolved enum and byte-order information; the
Exp-Golomb types have zero static bit width, default bit order, no enum
reference, and no equality constraint. A fixed array is expanded in source
order into typed fields named `name[0]` through `name[count - 1]`; every element
emits its own read and, when present, equality-check instruction. Conditional
blocks are lowered into the same declaration-order field stream. Each possible
field carries resolved presence guards that reference earlier typed-field
indexes; no jump opcode or general control-flow bytecode is introduced. Switch
case fields receive one positive equality guard. Default fields receive the
conjunction of every case guard negated; an omitted default emits no field for
the unmatched path. Nested switch and conditional guards are appended outer-to-
inner.

A repeat body is projected `maximum` times into that same typed-field stream.
Every field in iteration `i` appends a positive `count > i` guard after its
enclosing guards. Its materialized name appends the active repeat indexes before
any fixed-array index. Each projected repeat statement records its controller,
maximum, first body field, enclosing guards, and source range, and emits one
guarded `assert-repeat-count` at the statement position before the first body
read. This adds a greater-than presence comparison and a bound assertion, but
no jump, back edge, mutable index, or alternate source-coordinate operation.
Unsigned Exp-Golomb values are retained for repeat controllers without becoming
valid equality-conditional or switch controllers. A program with any parser or
compiler diagnostic has no executable typed IR.
`svtool rule check` runs both stages. The bundled Annex B runner also compiles its rule
once when the analyzer is created and executes the resolved structure index
for every record.

The compiler type-checks every pure function independently, then expands each
pure call into the computed field that uses it. The resulting typed expression
contains literals, previous typed-field indexes, and unary or binary operators;
there is no call opcode. A computed declaration joins the same typed-field
stream, retains its declared type and metadata, and inherits resolved outer
guards. It emits one `evaluate-computed` instruction, advances no static source
offset, and receives the same repeat-indexed materialized name and repeat-local
scope as a syntax field. Written expressions and expanded computed expressions
are rejected before executable typed IR is produced when they exceed their fixed
node, depth, or 4,096-step expansion-work bounds.

The minimum VM executes a structure by reading each field through the bounded
bit reader. Before executing bytecode, it verifies that the reader's complete
normalized backing exactly matches the execution mapping slice beginning at the
supplied logical start. A mismatched, reordered, missing, additional, or
out-of-range backing span is an invalid typed execution rejected before a
structure node is created or source data is read. A successful field becomes a
syntax-field node with its decoded value and logical range. Its source location
contains every forwarded source span resolved by the execution mapping and may
therefore cross source gaps without including them. For a little-endian field,
the VM reverses the significance of complete logical bytes after reading them;
it never changes the bit reader position or source mapping. A non-byte-aligned
absolute logical field start or first resolved source bit is an invalid typed
execution and does not consume that field. Later source-span boundaries need
not be byte-aligned. An enum field retains its numeric value and type metadata.
A value not declared by that enum retains the
field node, marks the structure invalid, and reports an `invalid-syntax`
diagnostic at that field. A truncated or failed read retains earlier fields and
marks the structure invalid with a source diagnostic. An `@equals` mismatch
retains the field, then marks the structure invalid with an invalid-syntax
diagnostic. Each expanded array element becomes a separate syntax-field node
whose source location covers only that element. A failure keeps all earlier
complete elements, creates no node for an incomplete element, and uses the
expanded path such as `Header.values[2]` in its diagnostic. A failure in a later
mapped backing span leaves the reader at the field start and creates no partial
field node. Diagnostics resolve the available logical range through the same
mapping and include only forwarded source spans. The executor retains the field
type, description, and specification reference on the analysis-node snapshot;
presentation derives field width from that node's logical range.

Before each field read, the VM validates and evaluates its presence guards in
outer-to-inner order. A false guard skips the field without consuming source
bits, creating an analysis node, enforcing enum membership, or applying an
`@equals` value check. Selected fields retain the existing value, diagnostic,
and source-location behavior, so the following selected field begins exactly
where the previous selected field ended. Guard metadata that references an
unknown, current, future, incorrectly typed, or unavailable controller is an
invalid typed definition. Field definitions are still validated when their
branch is absent, so malformed typed IR cannot hide behind a false guard.
Consequently, exactly one matching switch case is materialized, otherwise the
default arm is materialized when present, and a switch without a matching case
or default consumes no arm input.

At a repeat statement whose enclosing guards are true, the bound assertion
checks the saved controller value before any body input is consumed. A count
above `maximum` is never clamped: it retains the count field, marks the structure
invalid, and reports `invalid-syntax` at that field. A source-backed controller
retains its exact source location; a computed controller reports its field path
without a location. If an enclosing guard is false, the assertion and all body
fields are skipped without requiring the controller value. A projected
iteration is present exactly when `count > i`.
Only present iterations consume source bits or materialized-node slots; absent
iterations create no nodes or source locations and perform no enum or
`@equals` value check. A selected field otherwise keeps the existing value,
metadata, partial-result, and source-coordinate behavior, including diagnostic
paths such as `Header.value[1][0]`. Invalid repeat metadata, controller guards,
or assertion placement is an invalid typed definition rather than guessed
execution.

Before executing any bytecode, the VM validates every computed typed expression,
including its node and depth bounds, result and operand types, previous-field
indexes, dependency availability, and controller guards. At an
`evaluate-computed` instruction, a false presence guard skips expression
evaluation and node creation. The instruction still consumes instruction budget
and remains a cancellation point. A successful evaluation consumes no source
bits and creates one `AnalysisNodeKind::ComputedField` with its `bool` or
unsigned 64-bit value, metadata, and no `FieldLocation`. A runtime arithmetic
failure creates no computed node, retains earlier nodes, marks the structure
invalid, and reports `invalid-syntax` at the computed field path without a source
location. Malformed expression or controller metadata is an invalid typed
definition. Subexpression work is part of the one instruction and introduces no
additional cancellation point.

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
following NAL-unit span (an empty final unit has no payload span). The NAL-unit
span excludes the maximal `trailing_zero_8bits` run before the next start code
or end of source; the scanner exposes that framing run as a separate span. A
`00 00 00 01` prefix remains one four-byte start code; between two start codes,
any additional preceding zeros belong to the previous unit's trailing framing.
Prefixes may cross a window boundary. Cancellation is checked at least every
1,024 inspected source positions; already published records remain valid and
the batch reports `cancelled`. The NAL offset and zero length remain valid for
an empty unit even though its optional payload span is absent. A source whose
byte size cannot be represented by the 64-bit source-bit coordinate model is
rejected before the scanner reads it.

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
whose source location covers the start code, any non-empty NAL payload, and any
separately identified trailing-zero framing. Its `start_code` child covers only
the three- or four-byte prefix. A `NalUnitHeader` child consumes exactly the
first eight payload bits and exposes `forbidden_zero_bit`, `nal_ref_idc`, and
`nal_unit_type`.

After a successful direct header, an ordinary non-empty payload is mapped from
EBSP to an RBSP logical view without copying bytes. Each complete `00 00 03`
sequence excludes the `03` as an
`emulation_prevention_three_byte[index]`; adjacent forwarded bytes are
coalesced into source spans. The NAL children appear in this order:
`start_code`, `NalUnitHeader`, optional `rbsp_payload`, zero or more
`emulation_prevention_three_byte[index]` regions in source order, and optional
`trailing_zero_8bits`. NAL-unit types `14`, `20`, and `21` require extension
headers that this profile does not yet parse, so their bytes after the direct
header remain uninterpreted and are not passed to the mapper.

Annex B analysis batches have an independent positive mapped-byte budget in
addition to their record-count and inspected-position budgets. The default is
64 KiB of EBSP source bytes per call. Exhausting that budget reports
`in-progress` and preserves the mapper's committed prefix so a later batch can
resume the same NAL.

The mapper reports source-located conformance issues separately from the RBSP
transformation. It still excludes `03` from `00 00 03 xx` when `xx > 03`, and
reports that prohibited sequence; it forwards but reports `00 00 00`,
`00 00 01`, and `00 00 02`, and reports a non-empty payload whose final byte is
`00`. These issues retain the complete `rbsp_payload` and excluded regions,
mark only the affected NAL invalid, and do not prevent later NAL units from
being analyzed. Cancellation, source error, and resource-limit failures retain
the direct header and publish the committed RBSP prefix with the corresponding
state and diagnostic before terminalizing the NAL and root.

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

Valid equality-conditional example:

```cpp
enum PacketKind {
    compact = 1;
    extended = 2;
}

struct Packet {
    bits<2> kind @enum(PacketKind);
    if (kind == 1) {
        bits<3> compact_value;
    } else {
        bits<5> extended_value;
    }
    bits<3> tail;
}

entry Packet;
```

Valid equality-switch example:

```cpp
enum PacketKind {
    compact = 1;
    extended = 2;
}

struct Packet {
    bits<2> kind @enum(PacketKind);
    switch (kind) {
    case 1: {
        bits<3> compact_value;
    }
    case 2: {
        bits<5> extended_value;
    }
    default: {
        bits<4> unknown_value;
    }
    }
    bits<2> tail;
}

entry Packet;
```

Valid bounded-repeat example:

```cpp
struct SampleTable {
    bits<8> sample_count;
    repeat (sample_count, 16) {
        bits<16, little> value @description("Little-endian sample.");
        bits<8> flags[2];
    }
}

entry SampleTable;
```

Valid pure-function and computed-field example:

```cpp
pure bool between(u64 value, u64 low, u64 high) {
    return value >= low && value <= high;
}

struct NalUnitHeader {
    bits<5> nal_unit_type;
    computed<bool> is_vcl = between(nal_unit_type, 1, 5)
        @description("Video coding layer NAL unit.");
    computed<u64> next_type = nal_unit_type + 1;

    if (is_vcl) {
        bits<1> first_slice_flag;
    }
    switch (next_type) {
    case 6: {
        bits<1> follows_vcl_range;
    }
    }
}

entry NalUnitHeader;
```

Invalid minimum examples include `bits<0> flag;`, `bits<65> flag;`,
`bits<12, little> value;`, a little-endian field after an unaligned field,
`ue value @equals(0);`, `se value @enum(Type);`, a little-endian field after a
variable-length field, `bits<1> flags[0];`, `bits<1> flags[];`, an expression or
second dimension in an array length, a structure projection above 99,999
fields, a truncated array element, a truncated Exp-Golomb codeword, 64 leading
zero bits, `@enum(Missing)`, an enum member value that does not fit its field,
duplicate enum member names, a sequence without `@index(progressive)`,
`if (future == 1)` before `future` is declared, an array or `ue`/`se` condition
controller, a condition integer outside the controller width, a branch-local
controller used after its branch, `if (flag = 1)`, a switch over a future,
array, or `ue`/`se` controller, an out-of-range or duplicate case value, a
switch with no case, a repeated or non-final default, a missing case colon or
braced arm body, `break`, fallthrough, multiple labels for one arm, a case
range or enum member label, a repeat over an unknown, future, array, `se`, or
unavailable branch-local controller, `repeat (count, 0)`, a maximum that does
not fit its fixed-width controller, a count or maximum expression, an empty
repeat body, an annotation before `repeat`, a repeat projection above 99,999
fields, a repeat-local controller used by another iteration or after the
repeat, a little-endian field after a repeat, `scan(other_scanner)`, two
declarations with the same name, or a program with no `entry`.

Invalid pure-function and computed-field examples include an annotated pure
function, more than 16 parameters, duplicate parameter or function names, an
overload or top-level name collision, a pure body that calls a later function or
recurses, a nonparameter
value reference in a pure body, a return-type or call-argument mismatch, and a
malformed or missing return expression. They also include a computed array,
`computed<se>`, `@equals` or `@enum` on a computed field, a reference to an array,
`se`, future, unknown, or unavailable branch-local field, a mixed-type operator,
an unsupported operator or conversion, a computed expression or expanded pure
call above 256 nodes, depth 64, or 4,096 shared expansion steps,
`computed<bool>` in an equality condition or as a switch/repeat controller,
`computed<u64>` in the Boolean `if (flag)` shorthand, and a general expression
directly in an array length, case label,
repeat maximum, or repeat controller. Reached unsigned overflow or underflow,
division by zero, and remainder by zero are runtime `invalid-syntax` failures;
short-circuited failing operands are not evaluated.

A malformed repeat header produces source-ranged missing-token diagnostics. If
its body opener is missing, parsing recovers at the next field semicolon or
enclosing closing brace; missing header tokens otherwise continue into a
recognizable braced body when possible. Enum and field parsing recovers at the
next member/field semicolon or closing brace. An unknown switch label or a label
whose arm opener is missing recovers at the next `case`, `default`, or switch
closing brace. Malformed pure, parameter, return, call, expression, and
`computed<...>` syntax likewise produces source-ranged diagnostics. Statement
recovery stops at the next useful semicolon or enclosing closing brace;
expression recovery also treats the current call's comma or right parenthesis
as a boundary. All recovery preserves source ranges and diagnostics.

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
Computed fields have no `FieldLocation`, do not participate in source-bit
resolution, and are selected only through their analysis-tree nodes.

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

Conditional and switch syntax reserve no separate opcode or node budget. Every
possible field still emits its read instruction, and `@equals` still emits its
assertion instruction. Those instructions count toward the instruction budget
and remain cancellation points when the field is skipped. Only a selected
field consumes source bits and one materialized-node slot. All fields from all
branches and switch arms count toward the static 99,999-field projection
limit.

Each projected repeat statement adds one `assert-repeat-count` instruction.
Every read and equality assertion produced by the declared `maximum` counts
toward the instruction budget and remains a cancellation point even when its
iteration is absent. The bound assertion is also charged and remains a
cancellation point when its enclosing guards are false. Only present iterations
consume source bits and materialized-node slots. All projected fields count
toward the static 99,999-field limit, so a conservative maximum increases typed
program size and possible instruction work even when the decoded count is
small. A reached count above the declared maximum is `invalid-syntax`, not a
resource-limit condition.

Each computed field adds one `evaluate-computed` instruction. That instruction
counts toward the instruction budget and remains a cancellation point even when
a false guard skips evaluation. A successful evaluation consumes one
materialized-node slot and no source bits; a failed evaluation consumes no node
slot and retains earlier nodes. Pure calls add no runtime instructions because
the compiler inlines them. All subexpression work is charged within the one
instruction, with no extra cancellation point, and is bounded by the 256-node
and depth-64 expanded-expression limits. Computed fields also count toward the
static 99,999-field projection limit.

All limits must be greater than zero. The host may lower them for a particular
execution but a rule cannot raise or inspect them. The accepted minimum subset
contains no runtime calls or views: pure calls are statically inlined, and any
future runtime call or view operation must consume the already reserved depth
budgets. An instruction, node-count, or node-depth breach reports
`resource-limit`, retains nodes completed before the
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
