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

    assert(nal_unit_type != 5 || nal_ref_idc != 0) at nal_ref_idc;
    assert(nal_unit_type != 1 || nal_ref_idc == 0) at nal_ref_idc;

    computed<bool> is_vcl =
        nal_unit_type >= 1 && nal_unit_type <= 5;
}
```

The eventual reference must define primitive types, signedness, byte order, bit
order, overflow behavior, arrays, enums, structures, conditionals, switches,
bounded loops, pure helpers, scope, name resolution, and specification
annotations. The accepted minimum subset remains intentionally bounded:
expressions are accepted only in pure-function return values, computed fields,
lazy byte counts, dynamic bit widths, and assertion conditions. Imported values
remain restricted to dedicated dynamic-width, equality-guard, and
source-anchored assertion-condition forms, and control flow remains limited to
the conditional, switch, and bounded-repeat forms described below.

The accepted M3 type slice adds declaration-order enums and an explicit byte
order on `bits` fields. Enum declarations name unsigned integer values; a
`bits` or `ue` field uses `@enum(Type)` to associate those names with its
decoded value. Byte order changes numeric interpretation only. Source bit
addresses remain MSB-first and source locations remain the bits consumed by the
field.

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
optional. Equality accepts an earlier guaranteed scalar `bits`, enum, `ue`, or
`computed<u64>` controller; the Boolean shorthand accepts an earlier guaranteed
`computed<bool>`. Neither form is a general conditional expression.

The accepted switch slice adds nested
`switch (previous_field) { case integer: { ... } default: { ... } }` blocks.
Each arm has its own braced body, `default` is optional, and selection uses the
same restricted equality guards as the conditional slice.

The accepted bounded-repeat slice adds nested
`repeat (previous_count, maximum) { ... }` blocks. A decoded unsigned count
selects zero through `maximum` projected iterations. The positive literal
`maximum` bounds both the compiled field projection and the accepted runtime
count; this is not a general loop or count expression.

The accepted bounded-sentinel slice adds post-tested
`repeat (maximum) { ... } until (field == integer);` blocks. The body executes
at least once, includes the terminating item in the materialized syntax, and
uses a positive maximum in `1..64` to bound every projected iteration.

The accepted computed-value slice adds top-level expression-bodied `pure`
scalar functions and structure-local `computed<bool>` or `computed<u64>`
fields. Pure calls are statically inlined into a bounded typed expression;
computed fields consume no source bits and never receive a source location.
This adds no runtime call stack, mutable state, or host access.

The accepted lazy-boundary slice adds `@lazy(byte_count) bytes name;` regions.
The runtime validates their mapped logical range, creates a lazy analysis node,
and advances the logical cursor without reading or copying the payload. This
registers a safe uninterpreted boundary; typed on-demand expansion remains a
later slice.

The accepted progressive-index recovery slice keeps the existing
`@index(progressive)` H.264 sequence syntax and makes a cancelled index
resumable in the same analyzer. It preserves published nodes, scanner and queue
state, and monotonic identifiers without defining persistent checkpoints.

The accepted payload-dispatch slice adds a single top-level
`payload<rbsp> sequence switch (controller) { case integer: Structure; }`
declaration. It binds controller values decoded by a sequence element to the
structure that decodes the derived payload view, or to `empty` when that
payload carries no syntax elements. The rule, not the runner, decides which
structure a payload uses.

The accepted H.264 trailing-bits slice adds the terminal
`rbsp_trailing_bits;` structure item. It consumes the required stop bit and
the position-dependent zero padding of an RBSP without introducing a general
alignment expression or unbounded loop.

The accepted compressed-payload slice adds the named terminal
`compressed_payload name;` structure item. It publishes every bit remaining in
the bounded logical view as a materialized opaque payload and advances to the
view end without reading or copying those bits.

The accepted context-publication slice adds structure annotations
`@context` and repeatable `@context_dependency`, plus the scalar-field
annotation `@context_export`. A rules-owned execution session publishes
completed definitions and their selected typed values to a position-aware
directory without reading values back from presentation nodes.

The accepted context-import slice adds repeatable structure annotation
`@context_import`. After a consumer structure materializes, the same session
selects the declared generation at the consumer position and returns its
rules-owned export payload plus the exact dependency closure. Imported values
do not enter the general expression namespace.

The accepted imported dynamic-width slice permits a checked unsigned arithmetic
expression as the width of a big-endian `bits` field. The reserved
`context_value(import_key, context_kind, exported_field)` form resolves one
scalar from the exact imported generation closure before the field is read.

The accepted imported-condition slice also permits that reserved form as the
left side of `context_value(...) == integer`. It remains a `u64` leaf rather
than entering the general expression or controller namespace.

The accepted source-anchored assertion slice adds the structure statement
`assert(boolean_expression) at source_field;`. It lets a rule enforce a fatal
relationship between fields without creating a presentation node. The `at`
field supplies the diagnostic path and exact mapped source location.

The accepted imported-assertion slice additionally permits the reserved
`context_value(...)` leaf inside that Boolean condition. It uses the exact
imported generation contract and remains unavailable in other general
expression positions.

## Minimum DSL 0.1 Subset

The first executable subset uses the following grammar. Whitespace and `//` or
`/* ... */` comments may appear between tokens. Identifiers use ASCII letters,
digits, and `_`, but cannot begin with a digit. Integer literals are checked
unsigned decimal or `0x` hexadecimal values. String literals use `"`, `\\`,
`\n`, `\r`, and `\t` escapes.

```text
program       := { declaration }
declaration   := pure_function
               | { annotation } ( enum | struct | sequence | payload | entry )
pure_function := "pure" scalar_type identifier "("
                 [ parameter { "," parameter } ] ")"
                 "{" "return" expression ";" "}"
parameter     := scalar_type identifier
enum          := "enum" identifier "{" { enum_member } "}" [ ";" ]
enum_member   := identifier "=" integer ";"
struct        := "struct" identifier "{" { struct_item } "}" [ ";" ]
struct_item   := field | computed | lazy_region | assertion
               | rbsp_trailing_bits
               | compressed_payload
               | conditional | switch | repeat
field         := { annotation } field_type identifier [ "[" integer "]" ]
                 { annotation } ";"
field_type    := "bits" "<" additive [ "," identifier ] ">" | "ue" | "se"
computed      := { annotation } "computed" "<" scalar_type ">" identifier
                 "=" expression { annotation } ";"
lazy_region   := "@" "lazy" "(" expression ")" "bytes" identifier
                 { presentation_annotation } ";"
assertion     := "assert" "(" expression ")" "at" identifier ";"
rbsp_trailing_bits := "rbsp_trailing_bits" ";"
compressed_payload := "compressed_payload" identifier
                      { presentation_annotation } ";"
presentation_annotation := "@" "description" "(" string ")"
                         | "@" "spec" "(" string "," string ")"
scalar_type   := "bool" | "u64"
conditional   := "if" "(" ( identifier "==" integer | identifier
                               | context_value "==" integer ) ")"
                 "{" { struct_item } "}"
                 [ "else" "{" { struct_item } "}" ]
context_value := "context_value" "(" identifier "," identifier ","
                 identifier ")"
switch        := "switch" "(" identifier ")" "{"
                 switch_case { switch_case } [ switch_default ] "}"
switch_case   := "case" integer ":" "{" { struct_item } "}"
switch_default := "default" ":" "{" { struct_item } "}"
repeat        := "repeat" "(" identifier "," integer ")"
                 "{" { struct_item } "}"
               | "repeat" "(" integer ")" "{" { struct_item } "}"
                 "until" "(" identifier "==" integer ")" ";"
sequence      := "sequence" "<" identifier ">" identifier "="
                 "scan" "(" identifier ")" ";"
payload       := "payload" "<" identifier ">" identifier
                 "switch" "(" identifier ")" "{" payload_case { payload_case } "}"
payload_case  := "case" integer ":" ( identifier | "empty" ) ";"
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
  declaration namespace. Names are unique within a structure across syntax
  fields, computed fields, lazy byte regions, and compressed payloads, and a
  structure contains at least one of those items or one terminal
  `rbsp_trailing_bits` item.
- Enum member names are unique within their enum. Distinct members may name the
  same integer value; aliases accept the same decoded numeric value.
- A pure function declares a `bool` or `u64` return type, at most 16 uniquely
  named `bool` or `u64` parameters, and exactly one `return` expression. It may
  reference only its parameters and pure functions declared earlier. Function
  overloads, forward calls, direct or indirect recursion, and annotations on a
  pure function are rejected. Every body is type-checked and checked against
  the expression limits even when the function is unused.
- A literal `bits<N>` width is an integer in `1..64`. A non-literal width is a
  checked `u64` arithmetic expression and is accepted only for a big-endian,
  non-array `bits` field. Its runtime result must also be in `1..64`. Dynamic
  fields cannot use an enum, equality or range constraint, or serve as a context
  key, dependency, import, or export. Their runtime width makes the following
  exact static offset unknown. Fields consume input in
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
  An unsigned `ue` field may use one `@equals(integer)` constraint and one
  `@range(minimum, maximum)` constraint; a signed `se` field cannot use either.
- A scalar field may have one fixed array suffix `[count]`. `count` is an
  unsigned integer literal greater than zero; expressions, additional array
  dimensions, structure arrays, and runtime-sized arrays are not accepted.
  The declared base name remains the field name for duplicate-name checking.
  A structure may project at most 99,999 scalar fields after expansion, leaving
  one node for the structure within the default 100,000-node materialization
  budget. The compiler rejects a larger projection before producing executable
  typed IR.
- `@context(kind, key_field)` occurs at most once on a structure. `kind` is one
  of `h264-sps`, `h264-pps`, `aac-asc`, or
  `iso-bmff-sample-description`. `@context_dependency(kind, key_field)` is
  valid only on a structure that also has `@context`; it may occur at most 16
  times. An identical kind/field pair is a duplicate and is rejected rather
  than coalesced.
- Every context key and dependency key names an unconditional, top-level,
  non-array unsigned scalar declared in that structure. Accepted scalar kinds
  are `bits`, enum, `ue`, and `computed<u64>`. Signed fields, guarded or
  repeated fields, array elements, lazy regions, and generated trailing-bit
  fields are not context values.
- `@context_export` takes no arguments, occurs at most once on a field, and is
  valid only for the same unconditional unsigned scalar kinds in a structure
  with `@context`. One definition exports at most 64 values.
- `@context_import(kind, key_field)` may occur at most 16 times on a structure.
  It uses the same recognized kinds and unconditional unsigned scalar key-field
  rules. Declaration order is preserved; an identical kind/field pair is a
  duplicate and is rejected.
- `context_value(import_key, context_kind, exported_field)` is reserved for a
  dynamic `bits` width, the left side of an imported equality conditional, or
  the Boolean condition of a source-anchored assertion.
  All three arguments are identifiers. `import_key` names
  an earlier context-eligible field that identifies exactly one import on the
  structure. The kind identifier is `h264_sps`, `h264_pps`, `aac_asc`, or
  `iso_bmff_sample_description`, and must name the imported root kind or a kind
  reachable through its declared dependency graph. Exactly one structure must
  publish that target kind, and it must export exactly one field with the named
  declaration. Outside a dynamic `bits` width, a source-anchored assertion
  condition, and the exact conditional form `context_value(...) == integer`,
  it is rejected in pure-function bodies, computed fields, conditions, lazy
  sizes, switch or repeat controllers, and every other expression position.
- Fixed-width arrays contribute `width * count` bits to static alignment.
  Every element of a little-endian array must therefore have a byte-multiple
  width and the first element must begin at a structure-relative byte boundary.
  An array of `ue` or `se` fields has unknown total width, so a later
  little-endian field is rejected under the same rule as a scalar Exp-Golomb
  field.
- `rbsp_trailing_bits;` is an unannotated H.264 terminal item. It may occur
  once, only as the final top-level item of a structure; it is rejected in a
  conditional, switch, or repeat body and cannot be followed by another item.
  It reserves the names `rbsp_stop_one_bit` and `rbsp_alignment_zero_bit` in
  that structure. At runtime it reads a one-bit stop field constrained to `1`,
  then zero through seven one-bit alignment fields constrained to `0`, ending
  at the next logical-byte boundary. Each consumed bit is a separately named
  syntax-field node with its own mapped source location. Missing bits are
  `truncated-source`; a failed constraint is `invalid-syntax` at that field.
  The compiler reserves all eight possible fields against the 99,999-field and
  100,000-node limits, while the VM uses one bytecode instruction and publishes
  only the consumed alignment fields.
- `compressed_payload name;` is a named remaining-bit terminal. It may occur
  once, unconditionally as the final top-level item, and is mutually exclusive
  with `rbsp_trailing_bits`. It is rejected in conditional, switch, and repeat
  bodies, cannot have a leading annotation or array suffix, and accepts only
  trailing `@description` and `@spec`. Its name shares the structure-wide field
  namespace. At runtime it maps every bit remaining in the current bounded
  reader, publishes one materialized `CompressedPayload` node, and seeks to the
  end without reading or copying payload data. Non-byte-aligned, multi-span, and
  empty ranges are valid. The item has no scalar value and cannot be a
  controller, expression dependency, or context value.
- `assert(condition) at anchor;` is an unannotated, unconditional top-level
  structure item. It is rejected inside a conditional, switch, count repeat,
  or sentinel repeat, and it cannot follow a terminal item. `condition` must be
  `bool`; it uses the complete bounded expression and pure-function contract
  and may include the exact imported `context_value` leaf described above.
  Its local field dependencies must be earlier scalar unsigned `bits`, enum,
  `ue`, `computed<u64>`, or `computed<bool>` values guaranteed on the current
  path. Arrays, `se`, lazy regions, compressed payloads, unknown or future
  fields, and unavailable branch-local values are rejected as dependencies.
- The assertion anchor names an earlier source-backed, non-array scalar syntax
  field guaranteed on the current path. Fixed or dynamic `bits`, enum, `ue`,
  and `se` fields can anchor a diagnostic; computed fields and generated or
  region items cannot. An assertion is not a field, introduces no name or scalar value, does
  not affect static alignment or the 99,999-field projection, and cannot be a
  controller or context value. One structure declares at most 1,024 assertions.
- An equality-conditional controller must name an earlier scalar `bits`, enum,
  `ue`, or `computed<u64>` field guaranteed to have been materialized on every
  path reaching that condition. Arrays, `se`, `computed<bool>`, future or
  unknown fields, and a branch-local field used outside its guaranteeing branch
  are rejected. The integer must fit a source field's unsigned bit width;
  `computed<u64>` accepts the complete `u64` literal range. The shorthand
  `if (flag)` accepts only an earlier guaranteed `computed<bool>` and means
  equality with `true`. Conditions may nest, and both branches are statically
  checked even though only one executes. An imported equality instead has the
  exact form `context_value(import_key, context_kind, exported_field) == integer`;
  its value is `u64`, so the literal may use the complete `u64` range. It uses
  the static import, reachability, unique-publisher, and named-export contract
  above. General condition expressions, arithmetic around an imported value,
  Boolean combinations, imported shorthand, `!=`, ordering, `else if`, and
  other comparison forms are not accepted.
- Field names remain unique across all branches of a structure. Every possible
  branch field counts toward the 99,999-field projection limit. Static
  alignment is tracked independently through both branches; the conditional
  exit retains a known offset only when both paths end at the same known
  offset, with an omitted `else` treated as an empty path.
- A switch controller follows the equality conditional's declaration-order and
  path-availability rules and accepts scalar `bits`, enum, `ue`, or `computed<u64>`,
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
  Count expressions, EOF termination, and `break` are not accepted.
  A repeat body must contain at least one syntax field, computed field, or lazy
  byte region and may contain those item forms, conditionals, switches, and
  nested repeats.
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
- A sentinel repeat has the post-tested form
  `repeat (maximum) { ... } until (field == integer);`, with `maximum` in
  `1..64`. Its sentinel must be declared directly in the body as an
  unconditional, top-level, non-array fixed-width `bits`, enum, or `ue` field.
  Dynamic-width fields, `se`, computed fields, lazy regions, nested declarations,
  outside fields, and alternate comparisons are rejected. The value must fit
  the fixed width or supported `ue` domain. The body executes at least once and
  completes before the sentinel is tested; the terminating field is retained.
  A missing sentinel after the maximum iteration is runtime `invalid-syntax` at
  the final sentinel field. Fields from later projections are skipped without
  source access or nodes after termination. Local names do not escape, every
  projection counts toward 99,999 fields, and following static alignment is
  unknown. There is no `break`, `continue`, EOF, remaining-bit, or expression
  termination form.
- A computed field declares `computed<bool>` or `computed<u64>` and one
  expression. It may reference earlier scalar unsigned `bits`, enum, `ue`, or
  computed fields guaranteed on every path reaching the declaration. Arrays,
  `se`, unknown or future fields, and unavailable branch-local values are
  rejected. A computed field consumes zero bits, leaves static alignment
  unchanged, inherits enclosing guards, counts toward the 99,999-field
  projection limit, and is visible to later declarations under the same scope
  rules as a syntax field. Repeat projection appends the same indexes to its
  materialized name.
- A lazy byte region has the dedicated form
  `@lazy(byte_count_expression) bytes name;`. Its expression must produce
  `u64` and follows the computed-field rules for earlier scalar unsigned
  dependencies, path availability, pure-call expansion, and expression
  limits. The region is not a scalar value and cannot be referenced by a later
  expression or controller. Its name remains unique across the structure.
- In leading struct-item position, `@lazy(` is a reserved introducer parsed
  before the generic annotation list. Its argument uses the full expression
  grammar rather than the integer/string/identifier argument grammar of a
  normal annotation. Generic annotations cannot precede it.
- `bytes` is accepted only in that dedicated lazy form. A lazy region has no
  array suffix and accepts only trailing `@description` and `@spec`
  annotations. It inherits enclosing conditional, switch, and repeat guards;
  repeat projection appends the same indexes to its materialized name. Every
  projected region counts toward the 99,999-item structure limit.
- A lazy region must begin at a statically known byte boundary relative to its
  structure. Its runtime-sized byte count makes the following exact static
  offset unknown, so a later little-endian field or lazy byte region is
  rejected under the existing conservative alignment rule. The byte count is
  measured in logical bytes of the current view.
- Expressions accept unsigned integer and Boolean literals, identifiers, calls
  to declared pure functions, parentheses, unary `!`, checked `*`, `/`, `%`, `+`,
  and `-`, same-type `==` and `!=`, unsigned ordering, and short-circuit Boolean
  `&&` and `||`, with the precedence shown by the grammar. There are no implicit
  conversions: arithmetic and ordering require `u64`, logical operators require
  `bool`, equality operands have the same type, and function arguments exactly
  match their parameters. Unsigned overflow or underflow, division by zero, and
  remainder by zero are runtime `invalid-syntax` failures at the computed field,
  lazy region, dynamic field, or assertion anchor path. The same checked
  arithmetic applies to a dynamic bit width; `context_value` is its only
  additional leaf form and is also accepted as the exact left side of an
  imported equality conditional and inside a source-anchored assertion
  condition. It counts against the same node and depth limits. The complete
  width or assertion expression remains subject to the shared expansion-work
  limit. Enum fields contribute their decoded `u64`; enum member names are not
  expression values.
- Every written pure-function body, computed-field expression, lazy byte-count
  expression, dynamic width, or assertion condition, and every corresponding
  fully inlined expression, has
  depth at most 64 and at most 256 nodes. Expanding one body or expression may
  perform at most 4,096 shared work steps across calls, arguments, and parameter
  substitutions, including arguments for parameters the callee does not use.
  Pure calls are expanded statically and introduce no runtime call, recursion,
  source access, host access, time, randomness, or mutable state.
- The only accepted progressive sequence form is
  `@index(progressive) sequence<Element> name = scan(h264_start_code);`.
  `Element` must name a declared structure.
- A program declares at most one payload dispatch. Its view kind must be
  `rbsp`, and it must name a declared progressive sequence for which an `entry`
  exists. The controller must name an unsigned scalar `bits` field of at most
  64 bits declared unconditionally at the top level of that sequence's element
  structure, so it is guaranteed on every path. Exp-Golomb fields, computed
  fields, array elements, lazy regions, and any field inside a conditional,
  switch, or repeat body are rejected as controllers. Case values must be
  distinct and must fit the controller's declared width. Each case target names
  a declared structure other than the element structure, or `empty`. There is
  no `default` arm; an unlisted controller value keeps the uninterpreted
  payload behavior.
- An `@equals(integer)` field annotation is a checked constraint and may appear
  at most once on a `bits` or `ue` field. Its value must fit a `bits` field's
  unsigned bit width; a `ue` accepts `0..2^64 - 2`, its complete supported
  unsigned range.
- A `@range(minimum, maximum)` field annotation is a checked semantic
  constraint and may appear at most once, on a `ue` field only. Both arguments
  are unsigned integer literals satisfying
  `minimum <= maximum <= 2^64 - 2`. A field may carry both `@equals` and
  `@range`. Unlike every other checked constraint, a `@range` violation is not
  fatal: see the runtime semantics below.
  An `@enum(Type)` annotation may appear at most once on a `bits` or `ue` field
  and requires a declared enum type. Every declared member value must fit a
  `bits` field's bit width; a `ue` enum member must be in `0..2^64 - 2`. Enum
  values are still decoded as unsigned integers; the enum supplies names and
  fatal validation for the decoded value. A `ue` field may combine `@enum`,
  `@equals`, and `@range`: membership and equality are checked fatally before
  the non-fatal range bounds. `se` rejects all three annotations.
  `@description("text")` supplies project-authored presentation text, and
  `@spec("standard", "clause")` supplies a specification reference. Fields
  inherit their structure's specification unless they provide their own. An
  array declaration applies its resolved type, annotations, metadata, and
  constraints independently to every expanded element. A computed field accepts
  `@description` and `@spec`, before the declaration or after its expression,
  but rejects `@equals`, `@enum`, and an array suffix. A lazy byte region accepts
  those two presentation annotations only after its name.
- A source with lexical or static diagnostics produces no executable rule. The
  parser still returns its partial IR and all diagnostics with line/column
  ranges so an editor can report more than the first error.

`enum`, `big`, `little`, `ue`, `se`, `pure`, `return`, `bool`, `u64`,
`computed`, `lazy`, `bytes`, `true`, `false`, `if`, `else`, `switch`, `case`,
`default`, `repeat`, `until`, `assert`, `at`, `payload`, `empty`,
`rbsp_trailing_bits`, and `compressed_payload` are contextual words in the
positions shown by the grammar and remain ordinary identifiers elsewhere.
Existing scalar declarations are unchanged, and `bits<N>` remains exactly
equivalent to `bits<N, big>`; this slice deprecates no accepted 0.1 syntax.

The parser produces a source-oriented declaration model for diagnostics. The
static compiler resolves pure functions, enums, structures, sequences, and entry
references into a typed program, preserves declaration order, and emits
deterministic bytecode using `begin-structure`, `read-unsigned-bits`,
`read-unsigned-exp-golomb`, `read-signed-exp-golomb`, `evaluate-computed`,
`register-lazy-bytes`, `register-compressed-payload`, `read-rbsp-trailing-bits`, `assert-equals`,
`assert-range-minimum`, `assert-range-maximum`, `assert-repeat-count`,
`assert-sentinel-terminated`, `assert-expression`, and
`end-structure` operations. Each field opcode must match the resolved field type.
The fixed-width read carries the resolved enum and byte-order information; the
Exp-Golomb types have zero static bit width, default bit order, no enum
reference. Unsigned fields preserve optional equality and range constraints;
signed fields have neither. A fixed array is expanded in source order into typed
fields named `name[0]` through `name[count - 1]`; every element emits its own read
and, when present, constraint-check instructions. Conditional
blocks are lowered into the same declaration-order field stream. Each possible
field carries resolved presence guards that reference earlier typed-field
indexes; no jump opcode or general control-flow bytecode is introduced.
Context publication and import follow the same model: the compiler records each
structure's context definition, ordered dependencies, exported-value field
indexes, and ordered import kind/key indexes in typed IR. It emits no
context-specific opcode. Before any source read, the VM validates every context
index, scalar type, duplicate, and cardinality even for typed programs supplied
directly rather than produced by the compiler. After all selected fields
execute successfully, it returns only the declared publication values and
import keys with their exact locations to the execution session. Validation and
collection use the existing instruction, expression, node, and cancellation
budgets; context metadata cannot create an unbudgeted execution path.

`rbsp_trailing_bits` lowers to one `read-rbsp-trailing-bits` instruction and
eight generated typed-field slots: one `rbsp_stop_one_bit` and seven possible
`rbsp_alignment_zero_bit[index]` fields. The instruction validates those
generated names, types, constraints, and H.264 7.3.2.11 metadata before it
reads. It publishes only the stop bit and the padding needed to reach the next
logical-byte boundary, so unused slots create neither nodes nor source reads.
The instruction remains one budgeted cancellation point; its eight individual
one-bit reads and nodes are independently bounded.

`compressed_payload` lowers to one typed field and one
`register-compressed-payload` instruction. Before source access, the VM verifies
that it is the final field and opcode, has only presentation metadata, and has
no scalar-only properties. A selected instruction consumes one instruction,
one node slot, and one cancellation point. It maps the reader's complete
remaining range, appends a materialized compressed-payload node, and seeks to
the exclusive end without issuing a source read.

Switch
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
Unsigned Exp-Golomb values are retained for repeat, equality-conditional, and
switch controllers. A program with any parser or compiler diagnostic has no
executable typed IR.

A sentinel repeat is projected to the same linear stream. Iteration zero
inherits the enclosing guards; every later iteration additionally requires all
earlier projected sentinel fields to differ from the terminating value. Typed
IR records each iteration start and sentinel field, the assertion position,
termination value, enclosing guards, and source range. One
`assert-sentinel-terminated` instruction follows the projected body. The VM
validates this descriptor and every projection's guard prefix before reading
source, so malformed typed IR cannot make fields execute after termination.
`svtool rule check` runs both stages. The bundled Annex B runner also compiles its rule
once when the analyzer is created and executes the resolved structure index
for every record.

An assertion lowers to one declaration-order `DslTypedAssertion` descriptor
containing its typed Boolean condition, source-backed anchor field index,
statement-position field index, and source range. It does not join the typed
field stream. One `assert-expression` instruction refers to the descriptor by
index with immediate zero and executes at that statement position. Assertions
at one position retain source order. When positioned operations coincide, the
bytecode order is sentinel completion, expression assertion, repeat-count
assertion, then the next field. The VM validates all three descriptor streams,
operands, immediates, positions, and ordering before source access for a
structure containing expression assertions.

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

A dynamic `bits` field retains the existing `read-unsigned-bits` instruction and
stores a typed width expression instead of a literal width. An imported leaf is
lowered to the context-import ordinal, target definition kind, unique publishing
structure index, and ordered export index. Runtime code compares no field names.
The compiler proves the target kind is the import root or is reachable through
declared context dependencies and makes every following exact static offset
unknown.

An imported equality conditional lowers the same canonical imported leaf into a
field-presence guard with an expected `u64` literal and a positive or negated
sense. Both branches remain linear field projections; no jump or conditional
opcode is added. Guard identity includes the complete import, kind, publisher,
and export descriptor, so nested conditions over different imported fields do
not alias.

A lazy declaration joins the same typed-field stream with kind `LazyBytes`, a
required inlined `u64` byte-count expression, resolved presentation metadata,
and the same outer guards and repeat indexes. It emits one
`register-lazy-bytes` instruction. The declaration is not added to the scalar
value namespace, and its dynamic width makes the following exact static offset
unknown.

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
diagnostic. A `@range` violation instead keeps the structure and every later
field materialized: it retains the decoded value and attaches a warning
`invalid-syntax` diagnostic to that field node, reporting whether the value fell
below the minimum or above the maximum. Decoding continues because a
non-conformant `ue` value still consumed a well-formed codeword, so no
subsequent field position depends on the violation. A value equal to either
bound conforms and reports nothing. When a field carries both constraints, the
`@equals` check runs first, so a mismatch marks the structure invalid before any
range warning is attached. Each expanded array element becomes a separate syntax-field node
whose source location covers only that element. A failure keeps all earlier
complete elements, creates no node for an incomplete element, and uses the
expanded path such as `Header.values[2]` in its diagnostic. A failure in a later
mapped backing span leaves the reader at the field start and creates no partial
field node. Diagnostics resolve the available logical range through the same
mapping and include only forwarded source spans. The executor retains the field
type, description, and specification reference on the analysis-node snapshot;
presentation derives field width from that node's logical range.

Before a selected dynamic field reads source, the VM evaluates its prevalidated
width expression. `RuleExecutionSession` resolves an imported leaf by selecting
the root generation before the consumer's enclosing source-span start, caching
that exact rules-owned dependency closure for the run, and selecting the lowered
structure/export ordinal from it. Missing, future, or stale generations are
`dependency-unavailable` without fallback and mark the partial structure as
`waiting-dependency` at the import-key field. A missing or ambiguous target,
payload/schema mismatch, or malformed descriptor is an invalid definition.
Checked arithmetic failure or a width outside `1..64` is `invalid-syntax` and
consumes no bits for the dynamic field. A truncated read rolls back to the field
start, while its diagnostic retains the available mapped prefix.

Before any source read, the VM also validates every imported conditional guard
as one canonical `u64` leaf with no operands and a complete exact
import/publisher/export descriptor. When a guarded field is reached, the same
session resolver supplies the scalar from the per-run closure. A matching guard
selects the field; a nonmatching guard performs no source read, node creation,
or field constraint check. Resolver failures use the dynamic-width statuses and
are source-located at the import-key field. Even an unselected branch cannot
hide malformed typed guard metadata.

Before each field read, the VM validates and evaluates its presence guards in
outer-to-inner order. A false guard skips the field without consuming source
bits, creating an analysis node, enforcing enum membership, or applying an
`@equals` or `@range` value check. Selected fields retain the existing value, diagnostic,
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

At an `assert-expression` instruction, the VM evaluates the prevalidated
Boolean condition without reading source, moving the reader, or creating a
node. `true` continues at the next instruction. `false` immediately returns
fatal `invalid-syntax` with message `Assertion condition is false`, retains all
earlier materialized fields, and does not execute later fields. The diagnostic
path and location come from the `at` field's complete materialized range, so a
mapped anchor may preserve multiple disjoint forwarded source spans. Checked
arithmetic failures retain their existing `invalid-syntax` messages and use the
same anchor. The instruction itself remains an instruction-budget and
cancellation boundary; a limit or cancellation reached there prevents condition
evaluation and retains the same completed prefix.

Before executing any bytecode, the VM validates every computed or lazy typed
expression's metadata, including its node and depth bounds, result and operand
types, previous-field indexes, dependency availability, and controller guards.
Runtime range, mapping, and node-budget checks remain at the selected field
instruction. At an
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

A selected `register-lazy-bytes` instruction evaluates its typed `u64` byte
count, rejects checked-arithmetic or byte-to-bit overflow, and verifies that the
complete logical range fits the enclosing reader before it creates a node. It
resolves the range through the execution mapping. When a region crosses
excluded source bytes, it retains only its disjoint forwarded source spans. A
positive range becomes a `Region` in `lazy` state; a zero range becomes an empty
materialized `Region`.
Only after the location and node budget have been checked does the VM seek past
the range. It performs no payload read. A false guard skips expression
evaluation, node creation, and cursor movement.

An expression or byte-to-bit overflow is `invalid-syntax`; a declared range
larger than the reader's remaining enclosing range is `truncated-source` and
retains the available mapped prefix in its diagnostic. Misaligned execution,
missing or wrongly typed expression metadata, invalid references, mapping
failure, or opcode/type mismatch is an invalid typed definition. Cancellation
and resource-limit failures occur before node creation or cursor movement and
retain earlier completed fields.

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

The scanner owner may replace its cancellation token after a cancelled batch.
This does not reset the cursor, pending start code, trailing-zero run, inspected
count, or read window. A later batch therefore continues the same scan rather
than replaying records or rescanning the completed prefix.

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

After all eight header bits are available, the official rule asserts the clause
7.4.1 prerequisite `nal_unit_type != 5 || nal_ref_idc != 0` and the bounded
type-1 support prerequisite `nal_unit_type != 1 || nal_ref_idc == 0`. A type-5
header with zero reference priority, or a type-1 header with nonzero reference
priority, retains all three fields, fails with fatal `invalid-syntax` at the
exact two-bit `nal_ref_idc` span, and never maps or materializes that NAL's
`rbsp_payload`; scanning continues with the next NAL. The type-1 prerequisite
keeps unsupported non-IDR reference-picture marking from being interpreted as
later slice fields.

After a successful direct header, an ordinary non-empty payload is mapped from
EBSP to an RBSP logical view without copying bytes. Each complete `00 00 03`
sequence excludes the `03` as an
`emulation_prevention_three_byte[index]`; adjacent forwarded bytes are
coalesced into source spans. The NAL children appear in this order:
`start_code`, `NalUnitHeader`, optional `rbsp_payload`, zero or more
`emulation_prevention_three_byte[index]` regions in source order, and optional
`trailing_zero_8bits`. NAL-unit types `14`, `20`, and `21` require extension
headers that this profile does not yet parse, so their bytes after the direct
header remain uninterpreted, are not passed to the mapper, and cannot dispatch.

The bundled rule declares a payload dispatch for `nal_unit_type` values `1`,
`5`, `7`, `8`, `9`, `10`, and `11`. A dispatched type is always mapped, even when its
payload is empty, so `rbsp_payload` is present for every dispatched NAL. Type `9` decodes
`AccessUnitDelimiterRbsp` as a child of `rbsp_payload`, exposing
`primary_pic_type`, `rbsp_stop_one_bit`, and `rbsp_alignment_zero_bit[0]`
through `rbsp_alignment_zero_bit[3]` from its terminal
`rbsp_trailing_bits;` item. Types `10` and `11` are declared `empty`
and require a zero-length RBSP. A one-byte access unit delimiter is therefore
fully decoded, a header-only access unit delimiter is `truncated-source`, a
header-only end of sequence or end of stream is materialized, and either of
those types carrying RBSP bytes is `invalid-syntax`. Type `7` decodes the
8-bit Baseline/Main/Extended SPS core and the constrained High subset with 4:2:0 chroma,
eight-bit luma/chroma, transform bypass disabled, no scaling matrix, and
`pic_order_cnt_type == 0`. When `vui_parameters_present_flag` is one, the rule
also decodes the bounded Annex E.1.1 VUI core: aspect-ratio information including
Extended SAR, overscan information, video-signal and optional colour-description
fields, chroma sample locations, timing information, the NAL/VCL HRD presence
flags and complete bounded Annex E.1.2 HRD schedules, `low_delay_hrd_flag`,
`pic_struct_present_flag`, and the complete bitstream-restriction branch. The SPS
`rbsp_trailing_bits;` remains the exact terminal after the optional VUI.

Each present HRD flag selects an independently prefixed schedule with one through
32 CPB entries and four delay-length fields. Either presence flag causes the shared
`low_delay_hrd_flag` to be read after both schedules. Each `cpb_cnt_minus1` carries
a non-fatal `@range(0, 31)` warning, while its derived count controls
`repeat(..., 32)`. A larger count therefore retains the source-backed warning, then
stops the SPS before schedule entries at the layout-critical repeat boundary; the
scale fields and derived count remain available as a decoded prefix.

The two chroma sample-location types carry non-fatal `@range(0, 5)` constraints.
`max_bytes_per_pic_denom`, `max_bits_per_mb_denom`, and both
`log2_max_mv_length_*` fields carry non-fatal `@range(0, 16)` constraints. Both SPS
`log2_*_minus4` fields carry the clause 7.4.2.1.1 `@range(0, 12)` bounds. A violation
of these non-layout bounds keeps the field, complete SPS/NAL, and later declared
fields materialized, and reports a source-located `invalid-syntax` warning on that
field. Other syntax values remain source-backed, but materialization claims only
exact consumption of the selected declared branches, not complete Annex E semantic
conformance. Reserved fixed-width table values, nonzero SAR/timing values, timing
ratios, level-dependent HRD bitrate/CPB/delay relationships, and the relationship
between `max_num_reorder_frames`, `max_dec_frame_buffering`, and SPS-derived decoder
limits remain unchecked where the stable DSL lacks the required fixed-width or
relational constraint. A materialized SPS/VUI/HRD core publishes an SPS context
generation after exact RBSP consumption; it does not imply that later SEI timing
consumers have been decoded.

The declared VUI fields have the following bounded meanings:

| Field | Meaning in this slice |
| --- | --- |
| `vui_parameters_present_flag` | Selects the optional bounded VUI core before the SPS trailing bits. |
| `aspect_ratio_info_present_flag` | Signals `aspect_ratio_idc` and its optional Extended SAR dimensions. |
| `aspect_ratio_idc` | Identifies the sample aspect ratio; value 255 selects Extended SAR. Other table-value validity is deferred. |
| `sar_width` | Gives the 16-bit Extended SAR horizontal size; a nonzero requirement is deferred. |
| `sar_height` | Gives the 16-bit Extended SAR vertical size; a nonzero requirement is deferred. |
| `overscan_info_present_flag` | Signals `overscan_appropriate_flag`. |
| `overscan_appropriate_flag` | Indicates whether cropped display may be appropriate. |
| `video_signal_type_present_flag` | Signals video-format, range, and optional colour-description fields. |
| `video_format` | Identifies the source video format; reserved table-value checks are deferred. |
| `video_full_range_flag` | Selects full-range rather than studio-range sample values. |
| `colour_description_present_flag` | Signals the three colour-description identifiers. |
| `colour_primaries` | Identifies the source colour primaries; reserved table-value checks are deferred. |
| `transfer_characteristics` | Identifies the transfer characteristics; reserved table-value checks are deferred. |
| `matrix_coefficients` | Identifies the matrix coefficients; reserved table-value checks are deferred. |
| `chroma_loc_info_present_flag` | Signals top- and bottom-field chroma sample locations. |
| `chroma_sample_loc_type_top_field` | Identifies the top-field chroma location and warns outside `0..5`. |
| `chroma_sample_loc_type_bottom_field` | Identifies the bottom-field chroma location and warns outside `0..5`. |
| `timing_info_present_flag` | Signals the timing scale, tick count, and fixed-rate indication. |
| `num_units_in_tick` | Gives the 32-bit clock-tick numerator; the nonzero and ratio checks are deferred. |
| `time_scale` | Gives the 32-bit time scale; the nonzero and ratio checks are deferred. |
| `fixed_frame_rate_flag` | Indicates whether temporal distance between coded pictures is constrained. |
| `nal_hrd_parameters_present_flag` | Signals the complete bounded NAL HRD schedule. |
| `vcl_hrd_parameters_present_flag` | Signals the complete bounded VCL HRD schedule. |
| `hrd_parameters_present` | Derived Boolean used to select `low_delay_hrd_flag`; it has no source location. |
| `low_delay_hrd_flag` | Indicates the low-delay HRD mode when either HRD schedule is present. |
| `pic_struct_present_flag` | Signals picture-structure information for later timing consumers. |
| `bitstream_restriction_flag` | Signals the complete bounded bitstream-restriction branch. |
| `motion_vectors_over_pic_boundaries_flag` | Indicates whether motion vectors may cross picture boundaries. |
| `max_bytes_per_pic_denom` | Bounds the maximum coded-picture byte count and warns outside `0..16`. |
| `max_bits_per_mb_denom` | Bounds the maximum macroblock bit count and warns outside `0..16`. |
| `log2_max_mv_length_horizontal` | Gives the horizontal motion-vector length bound and warns outside `0..16`. |
| `log2_max_mv_length_vertical` | Gives the vertical motion-vector length bound and warns outside `0..16`. |
| `max_num_reorder_frames` | Gives the maximum number of frames that may precede an output frame; relational checks are deferred. |
| `max_dec_frame_buffering` | Gives the decoder frame-buffer bound; SPS-derived and relational checks are deferred. |

Each `*` below stands for the `nal_hrd` or `vcl_hrd` prefix of one independently
present HRD schedule:

| Field | Meaning in this slice |
| --- | --- |
| `*_cpb_cnt_minus1` | Gives one less than the number of CPB schedules, warns outside `0..31`, and is bounded by the repeat contract. |
| `*_bit_rate_scale` | Supplies the four-bit exponent used to derive schedule bit rates. |
| `*_cpb_size_scale` | Supplies the four-bit exponent used to derive schedule CPB sizes. |
| `*_cpb_count` | Derived `cpb_cnt_minus1 + 1` repeat count with no source location. |
| `*_bit_rate_value_minus1[i]` | Supplies the indexed CPB schedule bitrate value before scaling. |
| `*_cpb_size_value_minus1[i]` | Supplies the indexed CPB schedule size value before scaling. |
| `*_cbr_flag[i]` | Indicates whether the indexed schedule operates at a constant bitrate. |
| `*_initial_cpb_removal_delay_length_minus1` | Gives one less than the bit length of initial CPB removal delays. |
| `*_cpb_removal_delay_length_minus1` | Gives one less than the bit length of CPB removal delays. |
| `*_dpb_output_delay_length_minus1` | Gives one less than the bit length of DPB output delays. |
| `*_time_offset_length` | Gives the signed time-offset bit length; zero means no time-offset syntax. |

Type `8` decodes the clause
7.3.2.2 base PPS with one slice group and no PPS extension. Out-of-range PPS/SPS
identifiers or default reference-index counts retain the complete PPS with a
field warning. A nonzero `num_slice_groups_minus1`, reserved
`weighted_bipred_idc`, or extension syntax is `invalid-syntax` because the
unsupported input changes or extends the declared layout. A materialized PPS
resolves the most recent available SPS with the declared identifier before the
PPS NAL and binds that exact generation when it publishes its own generation.
If no SPS is available, the PPS structure remains materialized but receives a
source-located `dependency-unavailable` diagnostic; its RBSP and NAL are invalid,
nothing is published, and later NAL units are still analyzed. Generic context
import is used by the bounded type-5 IDR slice below. Every other type keeps the
uninterpreted `rbsp_payload` region unchanged.

The declared PPS fields have the following bounded meanings:

| Field | Meaning in this slice |
| --- | --- |
| `pic_parameter_set_id` | Identifies the PPS; clause 7.4.2.2 constrains it to `0..255`. |
| `seq_parameter_set_id` | Selects the most recent available SPS generation before the PPS NAL; clause 7.4.2.2 constrains it to `0..31`. |
| `entropy_coding_mode_flag` | Selects CAVLC or CABAC entropy coding for associated slices. |
| `bottom_field_pic_order_in_frame_present_flag` | Signals bottom-field picture-order syntax in associated slice headers. |
| `num_slice_groups_minus1` | Must be zero because this slice does not parse flexible macroblock ordering. |
| `num_ref_idx_l0_default_active_minus1` | Sets the default list 0 reference count and warns outside `0..31`. |
| `num_ref_idx_l1_default_active_minus1` | Sets the default list 1 reference count and warns outside `0..31`. |
| `weighted_pred_flag` | Enables weighted prediction for P and SP slices. |
| `weighted_bipred_idc` | Selects disabled, explicit, or implicit weighted biprediction; value 3 is reserved. |
| `pic_init_qp_minus26` | Sets the initial luma QP relative to 26; its SPS-dependent signed bound is deferred. |
| `pic_init_qs_minus26` | Sets the initial SP/SI QP relative to 26; its signed bound is deferred. |
| `chroma_qp_index_offset` | Sets the first chroma QP index offset; its signed bound is deferred. |
| `deblocking_filter_control_present_flag` | Signals deblocking-filter control syntax in associated slice headers. |
| `constrained_intra_pred_flag` | Restricts intra prediction to intra-coded neighboring macroblocks. |
| `redundant_pic_cnt_present_flag` | Signals redundant-picture count syntax in associated slice headers. |

Type `1` decodes `NonIdrAllISliceLayerWithoutPartitioningRbsp` for the bounded
progressive non-reference all-I `slice_type` values 2 and 7. The direct-header
assertion requires `nal_ref_idc == 0`, so the projection does not contain
`dec_ref_pic_marking`. It imports the same exact PPS/SPS generations as the IDR
shape and reads `first_mb_in_slice`, `slice_type`, `pic_parameter_set_id`,
`frame_num`, `pic_order_cnt_lsb`, optional bottom-field POC and redundant-picture
fields, `slice_qp_delta`, optional deblocking control, and the opaque
`slice_data` suffix. It omits the IDR-only `idr_pic_id`,
`no_output_of_prior_pics_flag`, and `long_term_reference_flag` fields.

Type `5` decodes `IdrSliceLayerWithoutPartitioningRbsp` for the bounded
progressive all-I `slice_type` values 2 and 7. It imports the exact PPS
generation selected by `pic_parameter_set_id` and that PPS's exact SPS
dependency, then reads the following fields:

| Field | Meaning in this slice |
| --- | --- |
| `first_mb_in_slice` | Identifies the first macroblock in the slice. |
| `slice_type` | Names the supported all-I form: `i = 2` or equivalent `all_i = 7`; other values are fatal at this codeword. |
| `pic_parameter_set_id` | Selects the exact prior PPS generation and warns outside `0..255`. |
| `frame_num` | Uses `log2_max_frame_num_minus4 + 4` bits from the bound SPS. |
| `idr_pic_id` | Identifies the IDR picture. |
| `pic_order_cnt_lsb` | Uses `log2_max_pic_order_cnt_lsb_minus4 + 4` bits from the bound POC-type-0 SPS. |
| `delta_pic_order_cnt_bottom` | Carries the signed bottom-field POC delta when the bound PPS enables it. |
| `redundant_pic_cnt` | Identifies the redundant representation when the bound PPS enables it; values outside `0..127` warn. |
| `no_output_of_prior_pics_flag` | Controls output of pictures preceding the IDR picture. |
| `long_term_reference_flag` | Marks the IDR picture as a long-term reference when set. |
| `slice_qp_delta` | Adjusts the initial luma quantization parameter; its signed bound is deferred. |
| `disable_deblocking_filter_idc` | Selects enabled, disabled, or within-slice deblocking when the bound PPS enables control syntax; reserved values are fatal. |
| `slice_alpha_c0_offset_div2` | Carries the signed alpha/c0 deblocking offset when filtering is not disabled; its signed bound is deferred. |
| `slice_beta_offset_div2` | Carries the signed beta deblocking offset when filtering is not disabled; its signed bound is deferred. |
| `slice_data` | Materialized opaque suffix covering every remaining RBSP bit, including any slice trailing bits; CAVLC/CABAC is not decoded. |

Dynamic widths still reject non-progressive SPS and POC types other than 0
before the affected field reads source. Exact imported PPS guards select
bottom-field POC, redundant-picture count, and deblocking-control fields; a
false guard consumes no bits and creates no node. Deblocking value 1 skips both
offsets, values 0 and 2 read them, and reserved values fail at the controlling
codeword. Missing/future/stale parameter-set generations remain
`dependency-unavailable`; the partial header is retained and later NAL units
are still analyzed. Reference type-1, P/B/SP/SI, field-picture, reference-list,
weighted, adaptive-memory-management, and slice-group branches are deferred.
Undispatched opaque fixtures use NAL type 12 now that type 1 is rule-owned.
Package `0.1.12` advertises coverage depth `all-i-slice-header`; this is not yet the
complete Baseline/Main/High slice-header milestone.

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
The type-5 reference-priority assertion likewise retains the complete direct
header, but stops before RBSP mapping and anchors its diagnostic to
`nal_ref_idc`.
Header read failures retain published nodes, mark the root invalid, and report
`source-error`. Cancellation retains completed NAL regions and marks the root
cancelled.

A cancelled Annex B analyzer may be resumed in place with an absent or fresh,
not-yet-requested cancellation token. Recovery is accepted only from the
cancelled terminal state. It removes the root's cancellation diagnostic,
returns the root to `indexing`, and retains the scanner cursor, queued records,
tree, node IDs, and next NAL/view identifiers. If the scanner reported the
cancellation, scanning continues from its pending boundary. If cancellation
was committed while decoding or mapping a NAL, that NAL and any mapped prefix
remain a cancelled partial result and are not retried; indexing continues with
the following record. Reaching end of source materializes the root even when a
cancelled descendant keeps the tree partial. Complete, source-error,
resource-limit, and invalid-rule results cannot be resumed. Persistent recovery
awaits source fingerprints, exact rule identity, and durable cache storage.

Valid minimum example:

```cpp
@spec("ITU-T H.264", "7.3.1")
struct NalUnitHeader {
    bits<1> forbidden_zero_bit @equals(0);
    bits<2> nal_ref_idc;
    bits<5> nal_unit_type;
    assert(nal_unit_type != 5 || nal_ref_idc != 0) at nal_ref_idc;
    assert(nal_unit_type != 1 || nal_ref_idc == 0) at nal_ref_idc;
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

Valid unsigned Exp-Golomb enum example:

```cpp
enum IdrAllISliceType {
    i = 2;
    all_i = 7;
}

struct SliceHeaderPrefix {
    ue first_mb_in_slice;
    ue slice_type @enum(IdrAllISliceType);
}

entry SliceHeaderPrefix;
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

Valid imported equality-conditional example:

```cpp
@context("h264-pps", id)
struct Pps {
    ue id;
    bits<1> optional_present @context_export;
}

@context_import("h264-pps", id)
struct Slice {
    ue id;
    if (context_value(id, h264_pps, optional_present) == 1) {
        se optional_value;
    }
    bits<1> tail;
}

entry Slice;
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

Valid bounded-sentinel example:

```cpp
struct RefPicListModifications {
    repeat (64) {
        ue modification_of_pic_nums_idc;
        if (modification_of_pic_nums_idc == 0) {
            ue abs_diff_pic_num_minus1;
        }
    } until (modification_of_pic_nums_idc == 3);
}

entry RefPicListModifications;
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

Valid payload-dispatch example:

```cpp
@spec("ITU-T H.264", "7.3.1")
struct NalUnitHeader {
    bits<1> forbidden_zero_bit @equals(0);
    bits<2> nal_ref_idc;
    bits<5> nal_unit_type;
    assert(nal_unit_type != 5 || nal_ref_idc != 0) at nal_ref_idc;
    assert(nal_unit_type != 1 || nal_ref_idc == 0) at nal_ref_idc;
}

@spec("ITU-T H.264", "7.3.2.4")
struct AccessUnitDelimiterRbsp {
    bits<3> primary_pic_type;
    rbsp_trailing_bits;
}

@index(progressive)
sequence<NalUnitHeader> nal_units = scan(h264_start_code);

@spec("ITU-T H.264", "7.3.1")
payload<rbsp> nal_units switch (nal_unit_type) {
    case 9:  AccessUnitDelimiterRbsp;
    case 10: empty;
    case 11: empty;
}

entry nal_units;
```

Valid context publication and import example:

```cpp
@context("h264-sps", sps_id)
struct Sps {
    ue sps_id;
    ue log2_max_frame_num_minus4 @context_export;
}

@context("h264-pps", pps_id)
@context_dependency("h264-sps", sps_id)
struct Pps {
    ue pps_id;
    ue sps_id;
    bits<1> entropy_mode @context_export;
}

@context_import("h264-pps", pps_id)
struct SliceHeader {
    ue first_mb_in_slice;
    ue pps_id;
    bits<context_value(pps_id,
                       h264_sps,
                       log2_max_frame_num_minus4) + 4> frame_num;
}

entry Sps;
```

Invalid minimum examples include `bits<0> flag;`, `bits<65> flag;`,
`bits<12, little> value;`, a little-endian field after an unaligned field,
`se value @equals(0);`, `se value @enum(Type);`, `se value @range(0, 12);`,
`bits<8> value @range(0, 12);`, `ue value @range(12, 0);`,
`ue value @range(12);`, `ue value @range(0, 1, 2);`,
`ue value @range(0, "twelve");`, a repeated `@range`, a little-endian field
after a
variable-length field, `bits<1> flags[0];`, `bits<1> flags[];`, an expression or
second dimension in an array length, a structure projection above 99,999
fields, a truncated array element, a truncated Exp-Golomb codeword, 64 leading
zero bits, `@enum(Missing)`, an enum member value that does not fit its field,
`ue value @enum(Type)` when `Type` contains `18446744073709551615`,
duplicate enum member names, a sequence without `@index(progressive)`,
`if (future == 1)` before `future` is declared, an array or `se` condition
controller, a condition integer outside the controller width, a branch-local
controller used after its branch, `if (flag = 1)`, a switch over a future,
array, or `se` controller, an out-of-range or duplicate case value, a
switch with no case, a repeated or non-final default, a missing case colon or
braced arm body, `break`, fallthrough, multiple labels for one arm, a case
range or enum member label, a repeat over an unknown, future, array, `se`, or
unavailable branch-local controller, `repeat (count, 0)`, a maximum that does
not fit its fixed-width controller, a count or maximum expression, an empty
repeat body, an annotation before `repeat`, a repeat projection above 99,999
fields, a repeat-local controller used by another iteration or after the
repeat, a little-endian field after a repeat, `scan(other_scanner)`, two
declarations with the same name, a program with no `entry`, or multiple `entry`
declarations.
Invalid sentinel-repeat examples include `repeat (0)` or `repeat (65)`, a
missing `until` clause, an unknown sentinel, a sentinel declared outside or in
nested control flow, an array, `se`, dynamic-width, computed, or lazy sentinel,
an out-of-range terminating value, and any comparison other than direct
equality with an integer literal.

Invalid assertion examples include a non-Boolean condition; a dependency or
anchor that is unknown, declared later, an array, or unavailable on the current
path; an `se` expression dependency; a computed, generated, lazy, compressed,
or array anchor; an assertion inside conditional, switch, or repeat control
flow; any leading annotation; missing parentheses, `at`, anchor, or semicolon;
a malformed `context_value` argument list, unresolved imported descriptor, or
bare imported `u64` condition; and a 1,025th assertion in one structure.

Invalid context-publication examples include a second `@context` on one
structure; an identical repeated `@context_dependency`; a dependency on a
structure without `@context`; a guarded, repeated, array, signed, lazy, or
generated key or export; `@context_export(1)`; more than 16 dependencies; and
more than 64 exports. Unknown context kinds and key names are rejected as well.
Invalid context-import examples include an identical repeated import, a guarded,
repeated, array, signed, lazy, generated, or unknown key field, an unsupported
kind, malformed arguments, and more than 16 imports.
Invalid imported dynamic-width examples include `context_value` outside a
dynamic `bits` width, imported equality conditional, or source-anchored
assertion condition; a missing or later import key; an unrelated target kind;
zero or multiple publishers for the target kind; a missing export; and a
dynamic little-endian, array, enum, constrained, context-key, dependency,
import, or export field. Runtime results `0` and `65`, arithmetic overflow or
underflow, division by zero, and remainder by zero are `invalid-syntax` before
that field consumes input.
Invalid imported-condition examples include arithmetic or a call around
`context_value`, `!=`, ordering, a Boolean combination, imported Boolean
shorthand, a nonliteral right side, a missing or later import key, an unrelated
target kind, zero or multiple publishers, and a missing export. Imported values
remain invalid as switch or repeat controllers, sentinel conditions, computed
or lazy expressions, array lengths, annotations, and payload dispatch values.

Invalid payload-dispatch examples include two payload declarations,
`payload<ebsp>` or any other view kind, a dispatch naming a structure or an
undeclared name instead of a sequence, a dispatch whose sequence has no
`entry`, an unknown controller name, an Exp-Golomb, computed, array-element, or
lazy-region controller, a controller declared inside a conditional, switch, or
repeat body, a case value outside the controller's width, a duplicate case
value, a dispatch with no case, a case target that is undeclared or is the
sequence element structure itself, a `default` arm, and a missing case colon or
semicolon.

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

Rules explicitly declare safe boundaries for content that can be materialized
later. The accepted lazy-byte slice registers an uninterpreted logical-byte
range without reading it:

```cpp
struct Packet {
    bits<16> payload_size;

    @lazy(payload_size)
    bytes payload @description("Deferred packet payload");
}
entry Packet;
```

The expression is evaluated only when the declaration's guards are selected.
The VM checks byte-to-bit conversion, logical alignment, the enclosing reader
limit, mapping resolution, and the node budget before it appends the region or
moves the cursor. It never reads the registered payload. A positive range is
`lazy`; an empty range is immediately `materialized`. The location contains the
exact mapped logical range and may contain multiple source spans.

Examples rejected statically include a Boolean byte count, a future or
branch-unavailable dependency, a lazy region after a variable-width field whose
alignment is unknown, `bytes payload;` without `@lazy`, and `@equals` or an array
suffix on a lazy region. Runtime arithmetic overflow is `invalid-syntax`; a
range beyond the enclosing reader is `truncated-source`, with no lazy node or
cursor movement.

The lazy slice registers only the checked boundary. Nested typed content,
decode recipes, and user-triggered subtree expansion are not yet accepted. The
accepted progressive index publishes structures in bounded batches and can
resume a cancelled scan in the same analyzer. The only current progressive form
is the H.264 start-code sequence:

```cpp
@index(progressive)
sequence<NalUnit> nal_units = scan(h264_start_code);
```

The analysis model distinguishes lazy, indexing, cancelled, unsupported,
invalid, and completely materialized states.

Index recovery preserves the append-only tree and never replays an already
published node in a later batch. It removes only stale cancellation diagnostics
from the analysis root representing the entry sequence. A NAL committed as a
cancelled partial result remains cancelled, so a root that later reaches
`materialized` may still belong to a tree with partial results. This in-memory
recovery is not a serialized or cross-process checkpoint contract.

## Payload Dispatch

A sequence element decodes a header; the syntax that follows it usually depends
on a value that header just produced. A payload dispatch binds those values to
the structures that decode the derived payload view:

```cpp
@spec("ITU-T H.264", "7.3.1")
payload<rbsp> nal_units switch (nal_unit_type) {
    case 9:  AccessUnitDelimiterRbsp;
    case 10: empty;
    case 11: empty;
}
```

The declaration is top-level and at most one may appear. `rbsp` is the only
accepted view kind; it names the mapped payload view the runtime already
derives for each sequence element. Annotations before the declaration become
the dispatch's own metadata.

The dispatch adds no opcode. A selected structure is executed by the same
`begin-structure` through `end-structure` bytecode the compiler emits for every
declared structure, so a case target has no special typed form. Case
distinctness, controller resolution, and target indexes are validated before
execution; a malformed dispatch is an invalid typed definition.

At runtime the controller value is read from the element's published header
after that header materializes.

An unlisted value changes nothing. The payload stays an uninterpreted region
when it is non-empty, and an element with no payload gets no payload node.

A listed value always receives the derived payload view, including when that
view is empty. Presence of a case, not payload length, decides whether the view
exists. A rule that describes a payload therefore always gets an exact view to
decode against.

An `empty` case requires the payload view's logical length to be exactly zero.
A non-empty payload is `invalid-syntax` at the payload path and retains the
complete payload region and every excluded region.

A structure case executes its target over the payload view from logical zero,
parented under the payload region node, with the same execution options,
sandbox budgets, and cancellation points as the header.

A materialized structure must consume the payload view's complete logical
length. Residual bits are `invalid-syntax` at the payload path. Exact
consumption is what makes trailing-bit declarations verifiable and what
prevents silently accepting bytes no declaration describes.

A structure needing more bits than the view holds is `truncated-source`,
including when the view is empty. Under the bundled H.264 rule a header-only
access unit delimiter is therefore truncated, while a header-only end of
sequence is materialized.

Payload failure marks the containing element invalid or cancelled and retains
the header, the payload region, the excluded regions, and any framing regions.
It never terminates the sequence, and later elements continue to be analyzed.

## Position-Aware Context Directories

`RuleExecutionSession` owns one exact compiled program, one format-neutral
context directory, and the rules-owned typed payloads published by one analysis
source and tree. Its first valid execution locks that analysis identity; reuse
with another source or tree is invalid rule/runtime state. Moving the session or
its containing analyzer preserves the identity and published generations. The
accepted kinds are H.264 SPS and PPS, AAC AudioSpecificConfig, and ISO BMFF
sample descriptions. A key also contains a numeric scope so equal
sample-description or parameter-set values in different tracks do not collide.
Standalone Annex B uses scope zero.

The session can execute a complete logical view or the suffix beginning at a
nonzero logical start. Its reader is backed by exactly that mapped slice, source
locations retain the original logical coordinates, and exact consumption means
consuming that slice rather than the mapping prefix. For a context definition or
import, every mapped source span in that slice must lie inside the non-empty
enclosing source span; a mismatch is rejected before source reads or analysis
binding. A structure without context annotations uses the same execution path
but publishes no directory effect.

The compiler lowers every declared key, dependency, export, and import key to a
stable typed-field index. Before reading source, the VM validates those indexes
and types. It returns only the selected values and their exact source locations,
not its full local environment; neither the session nor the analyzer walks
presentation-tree children to recover runtime values.

A definition is selectable only at source positions at or after its complete
source span's exclusive end. Among such definitions for the same key, lookup
selects the one ending nearest the query position. Registration may occur out of
source order, but same-key spans cannot overlap and stable definition IDs follow
append order.

A dependent definition binds exact dependency-generation IDs selected before
its own source span starts. At a consumer position, every dependency must still
resolve to that same generation. A later redefinition makes the dependent
context unavailable; the runtime neither falls back to an older requested-key
generation nor guesses. Malformed definitions are not registered, rejected
registration is transactional, and later cross-generation dependency cycles
or dependency chains beyond 64 definitions produce dependency-unavailable
results.

An import resolves at the consumer enclosing span's start when a dynamic width
first requests one of its values, or after the consumer materializes when no
earlier value request occurred. The exact closure is cached for that run and is
also the closure returned after successful exact consumption. A missing, future,
or stale generation adds a source-located `dependency-unavailable` diagnostic to
the import key and returns no partial imported result. A successful import
returns the root definition first, then its exact dependencies in
declaration-order depth-first traversal, with each definition included once.
Every entry retains the definition ID, kind, publishing structure index,
ordered exported values, and exact dependency IDs. A closure contains at most
64 definitions; a missing rules-owned payload is invalid runtime state. Import
results create no analysis nodes. Imported values are available only through
the lowered `context_value` leaf in a dynamic width or the exact
`context_value(...) == integer` conditional, or in a source-anchored assertion
condition. They are not identifiers in general expressions, lazy sizes, switch
or repeat controllers, or repeat bounds.

Publication occurs only after successful materialization, requested exact
consumption, dependency resolution, and complete typed-payload preparation.
Payload and directory capacity are reserved before registration, so committing
the prepared payload after successful directory mutation is a non-allocating
move under the single-writer model. Malformed, truncated, cancelled,
resource-limited, dependency-unavailable, or residual-bit executions publish
nothing. A failed redefinition therefore does not hide the previous valid
generation.

The directory owns only key, span, analysis-node, and dependency identity. The
rule owner retains the typed format payload. It performs no source reads and
follows the single-writer analysis-worker model. See
[ADR-0028](../adr/0028-resolve-context-generations-by-source-position.md).
The publication contract is specified by
[ADR-0044](../adr/0044-publish-rule-declared-context-generations.md). The
bundled H.264 rule first uses it in package version `0.1.7`. The import contract
is specified by
[ADR-0045](../adr/0045-import-rule-declared-context-generations.md); the bundled
rule does not use imports until slice dispatch is added. Dynamic imported widths
are specified by
[ADR-0046](../adr/0046-evaluate-dynamic-bit-widths-from-imported-context-values.md).
Imported equality guards are specified by
[ADR-0052](../adr/0052-guard-fields-with-imported-context-values.md).
Bounded post-tested sentinel repeats are specified by
[ADR-0047](../adr/0047-lower-bounded-sentinel-repeats-to-guarded-projections.md).
The compressed remaining-bit terminal is specified by
[ADR-0048](../adr/0048-register-a-compressed-remaining-bit-payload-terminal.md).
Source-anchored assertion statements are specified by
[ADR-0054](../adr/0054-add-source-anchored-assertion-statements.md).
Imported values in source-anchored assertions are specified by
[ADR-0056](../adr/0056-allow-imported-context-values-in-source-anchored-assertions.md).
The bounded progressive non-IDR all-I slice is specified by
[ADR-0055](../adr/0055-add-bounded-progressive-non-idr-all-i-slice-header.md).

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

One structure declares at most 16 context imports. Import selection performs no
source reads and creates no nodes. Each returned exact dependency closure is
bounded to 64 definitions; exceeding that bound is `resource-limit` and exposes
no partial imported result.

Dynamic-width imported leaves reuse that per-run closure and add no instruction,
source read, or presentation node. The complete expanded width expression remains
bounded to 256 nodes and depth 64; it is evaluated within the selected field's
single read instruction and existing cancellation boundary.

Array syntax does not reserve a separate runtime budget. Every expanded
element consumes one materialized node and one read instruction; `@equals`
adds one assertion instruction per element, and `@range` adds two, one per
bound. Truncation, a failed constraint,
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

Each sentinel repeat adds one `assert-sentinel-terminated` instruction after
all projected field instructions. Skipped projections still charge their field
instructions and cancellation points, but consume no source bits or nodes. The
assertion is charged even when enclosing guards are false. If no selected
sentinel equals the terminating value, the assertion returns `invalid-syntax`
at the final sentinel field while preserving the bounded materialized prefix.
The language-wide maximum of 64 bounds descriptor, guard, and assertion work.

One structure contains at most 1,024 source-anchored assertions. Each assertion
adds one descriptor and one `assert-expression` instruction, which consumes one
instruction-budget unit and is a cancellation point. Its fully inlined
condition is evaluated within that instruction and remains bounded to 256
expression nodes, depth 64, and the 4,096-step compile-time expansion limit.
The assertion adds no presentation node or source read and does not count
toward the 99,999-field projection.

Each computed field adds one `evaluate-computed` instruction. That instruction
counts toward the instruction budget and remains a cancellation point even when
a false guard skips evaluation. A successful evaluation consumes one
materialized-node slot and no source bits; a failed evaluation consumes no node
slot and retains earlier nodes. Pure calls add no runtime instructions because
the compiler inlines them. All subexpression work is charged within the one
instruction, with no extra cancellation point, and is bounded by the 256-node
and depth-64 expanded-expression limits. Computed fields also count toward the
static 99,999-field projection limit.

Each projected lazy byte region adds one `register-lazy-bytes` instruction.
The instruction counts toward the instruction budget and remains a cancellation
point even when a false guard skips it. A selected declaration evaluates its
already bounded expression within that instruction and consumes one node slot
only after its complete mapped boundary has been checked. Seeking over the
region performs no source read. Lazy regions count toward the static
99,999-item projection limit.

Each compressed payload adds one `register-compressed-payload` instruction,
one materialized-node slot, and one cancellation point. The instruction maps
and seeks across the complete remaining reader range without source reads. The
terminal counts as one item toward the static 99,999-field projection limit.

Resuming a cancelled progressive index consumes no source work, node, or batch
budget by itself. Every later batch uses the same positive record-count,
inspected-position, and mapped-byte limits and cancellation intervals as an
ordinary batch.

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

A rule package is versioned and declares its format identity, engine compatibility, applicability metadata, and dependencies. Format detection recommends candidates; the user may always override the selection. An analysis session records the exact selected package ID, version, content hash, and entry-point ID.

Application, language, and rule-package versions are independent. Package manifests declare an exact language contract and an engine compatibility range. During the `0.x` language phase, documented breaking changes are permitted; after language `1.0`, incompatible changes require a new major language version. An incompatible package is rejected with a diagnostic rather than interpreted heuristically.

First-release packages are self-contained, declare an empty dependency list,
and cannot resolve dependencies from the network at analysis time. Package
metadata never changes sandbox permissions or establishes trust.

Official rules are bundled with a particular application release. Additional packages may be installed only from a local file or directory in the first release; there is no online marketplace, automatic download, or automatic update. Installation presents package identity, version, format coverage, author metadata, content hash, and compatibility range. Saved sessions pin the complete selected package and entry-point identity, and installed rules receive no additional permissions based on claimed author or trust status.

### Package Layout

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

For local installation, the directory may be encoded as a deterministic
store-only ZIP32 container with the `.svrule` extension. The version 1
`rule.toml` manifest identifies the package, authors, license, package version,
exact language contract, half-open engine compatibility range, entry points,
declared format/profile/depth coverage, host detector metadata, and localized
documentation. Different package versions coexist; one package ID and version
cannot silently name different content.

Packaged rules contain no native executable code or symbolic links. Installers
reject absolute paths, parent traversal, duplicate or noncanonical paths,
Unicode and case-fold aliases, special files, and entries escaping the package
root. Validated content is retained read-only by a SHA-256 of the complete
logical package tree, so directory and archive forms share one identity. The
exact schema, identity framing, catalog behavior, canonical archive records,
and limits are defined by the normative
[rule-package format](../rule-packages.md).
