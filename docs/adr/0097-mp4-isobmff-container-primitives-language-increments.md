# ADR-0097: MP4/ISOBMFF Container Primitive Expressibility and Language Increments

- **Status**: Proposed
- **Date**: 2026-08-17
- **Authors**: StreamView Contributors

---

## Context

Phase 5 requires non-fragmented MP4/ISOBMFF container parsing (`video/mp4`, `audio/mp4`, ISO/IEC 14496-12 / 14496-14 / 14496-15). ADR-0096 (P5a) fixed the architectural boundaries — D1 box traversal and `mdat` lazy encapsulation, D2 cross-layer navigation (`avcC`/`esds` to H.264/AAC), D3 sample-table windowing — but its D1 DSL fragment referenced language constructs that do not exist. This ADR (Task P5c, corrected in P5c-R) probes what the current DSL/runtime can and cannot express for ISOBMFF container structure and fixes the **precise, directly codeable and testable contracts** for every capability increment. It authors **no rules**: all rule assets are deferred to P5d+ per the "enumeration/drilling mechanism unresolved ⇒ no MP4 rule assets" gate.

The seven container facts (plan facts 10–16, measured by the main agent on 2026-08-16 with `svtool rule check`) are incorporated without re-probing and re-confirmed where a probe was run this round:

1. **FourCC matching is expressible today**: `bits<32> box_type @equals(0x66747970)` compiles (`Rule OK`). No new `bytes` value type is needed.
2. **`bits<64>` is allowed** — `largesize` is directly readable; the field-width ceiling is 64 bits (`Bit field width must be in the range 1..64`, `dsl.cpp:1038`).
3. **`size == 1` / `else` payload sizes are expressible**, but only with `computed` and `@lazy` placed **inside each `if`/`else` branch** (a single cross-branch `computed` fails with `Computed dependency is not guaranteed on the current branch`).
4. **`size == 0` (extend to EOF) is the only true language gap**: `compressed_payload` is the sole "consume the rest" terminal but is constrained to the final top-level item (`error: compressed_payload must occur once as the final top-level item`, `dsl.cpp:1589`), and no `available_bytes` builtin exists.
5. **`maximumExpandedFieldsPerStructure = 99'999` per struct** (`dsl_ir.cpp:13`) bounds per-struct field expansion; sample tables above the bound must use lazy-region windowing.
6. **The only hierarchy mechanism today is a top-level `sequence` plus a single `payload<rbsp>` dispatch** (the H.264 shape). Nested box enumeration is the hard blocker this ADR resolves.
7. **`bits<128>` is not expressible** (probe: `error: Bit field width must be in the range 1..64`, `dsl.cpp:1038`); a 128-bit value is declared as two `bits<64>` fields.

All probes were executed this round with the working-tree tool binary `build/dev/tools/svtool/svtool` (`svtool 0.1.0 (DSL 0.1)`); scratch sources live outside the repository at `/Users/yun/.gemini/antigravity-cli/brain/12458dc0-7cd4-40c3-b0af-86d27dcb7b62/scratch/`. Results (full sources, commands, and outputs are reproduced in the P5c-R task report; the matrix below lists each command and result):

| Probe | Command | Result (verbatim error / status) | Live location |
| :--- | :--- | :--- | :--- |
| P1 `scan(mp4_box)` | `svtool rule check scratch/p5c_p1_scan_mp4_box.svfmt` | `error: Only h264_start_code and adts_frame are supported` | parser `dsl.cpp:3568`, IR `dsl_ir.cpp:3679` |
| P2a struct-typed field | `svtool rule check scratch/p5c_p2a_struct_typed_field.svfmt` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` | field-type parser |
| P2b `sequence` inside struct | `svtool rule check scratch/p5c_p2b_nested_sequence.svfmt` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` | sequence is top-level only |
| P2c `payload<mp4>` view kind | `svtool rule check scratch/p5c_p2c_payload_mp4_kind.svfmt` | `error: The only accepted payload view kind is rbsp` | parser `dsl.cpp:3669`, IR `dsl_ir.cpp:3717` |
| P2d `@container` | `svtool rule check scratch/p5c_p2d_container_annotation.svfmt` | `error: Unknown annotation '@container'` | `dsl.cpp:1880` |
| P5a `@target_format` post-position lazy | `svtool rule check scratch/p5c_p5a_target_format_lazy.svfmt` | `error: Lazy byte regions accept only @description and @spec` | `dsl.cpp:1895` |
| P5b `@target_format` pre-position | `svtool rule check scratch/p5c_p5b_target_format_preposition.svfmt` | `error: Expected bytes after @lazy(...)` | `dsl.cpp:1112` |
| P7a `unsupported("")` | `svtool rule check scratch/p5c_p7a_unsupported_empty_reason.svfmt` | `error: Unsupported statements require a non-empty reason` | `dsl_ir.cpp:2748` |
| P7b `unsupported` on computed anchor | `svtool rule check scratch/p5c_p7b_unsupported_computed_anchor.svfmt` | `error: Unsupported anchors require a source-backed scalar field` | `dsl_ir.cpp:2771` |
| P7c `unsupported` inside `repeat` | `svtool rule check scratch/p5c_p7c_unsupported_repeat_local.svfmt` | `error: Unsupported statements cannot be repeat-local items` | `dsl_ir.cpp:2741` |
| P7d `unsupported` after box header (moof shape) | `svtool rule check scratch/p5c_p7d_unsupported_positive.svfmt` | `Rule OK` | — |
| P8a uuid marker, two `bits<64>` usertype fields | `svtool rule check scratch/p5c_p8_uuid_marker.svfmt` | `Rule OK` | — |
| P8b `bits<128>` field | `svtool rule check scratch/p5c_p8_bits128.svfmt` | `error: Bit field width must be in the range 1..64` | `dsl.cpp:1038` |
| P8c `@container(Child)` post-position | `svtool rule check scratch/p5c_p8_container_annotation.svfmt` | `error: Unknown annotation '@container'` (name gate; argument `Child` parses as `DslAnnotationValueKind::Identifier`, `dsl.cpp:801`) | `dsl.cpp:1880` |

---

## Decision

### D1: Top-Level Box Enumeration — New `DslScannerKind::Mp4Box` with a Locked Framing Contract

The scanner-kind closed set `{H264StartCode, AacAdtsFrame}` (`dsl_ir.h:219-221`) is extended with `Mp4Box`. The new scanner is **framing-only**, symmetric to `AacAdtsScanner` (which recognizes the `0xFFF` syncword and the `aac_frame_length` chain but no profile semantics). It **must not recognize any specific FourCC**: "which FourCC means what" is format semantics that stays in the DSL rule (`bits<32> type @equals(0x...)` dispatch).

**Legal size set at each box start** (single-box legality):

1. `size == 0` — the box extends to EOF of the current region and is **terminal** (no subsequent box exists in that region).
2. `size == 1` — large-size box; the 16-byte header is required and the legal constraint is `largesize >= 16`.
3. `size >= 8` — normal box with the 8-byte header.
4. Any other value (`size` in 2..7; or `size == 1` with `largesize < 16`) is **malformed**: not a candidate box, and the position does not start a box.

**Header lengths and payload anchors**:

- Normal header: 8 bytes (`size` + `type`).
- Large header: 16 bytes (`size` + `type` + `largesize`).
- `uuid` box: after the 8/16-byte header, an additional 16-byte `usertype`; the rule-level payload anchor shifts by +16 (the scanner frames by `size` and is transparent to `uuid`). Minimum legal box size: normal 8, large 16, uuid-normal 24, uuid-large 32.

**Edge behaviors** (all defined, mirroring the ADTS scanner precedents):

- **Truncated header**: fewer than 8 bytes remain at a box start → the region ends; scanning stops; the trailing partial bytes are not a candidate (ADTS trailing-garbage precedent).
- **Span out of bounds**: `start + header_len + size > region_end` (or `largesize`) → the box is truncated: the span clamps to the region end and the box is flagged truncated; the DSL lazy region then hits the existing VM lazy truncation contract (`TruncatedSource`, `"Lazy byte region exceeds the available source range"`, `dsl_vm.cpp:3249-3252`). A truncated final box with a well-formed header still counts toward a candidate chain.
- **Integer addition overflow**: `start + header_len + size/largesize` is computed in checked 128-bit space; if `largesize > region_end - start` (or the sum would exceed the region), the box is out-of-bounds and handled as truncation/malformed above; no wrap-around is ever accepted.
- **size < header length**: covered by the legal size set (item 4).
- **Region end**: the scanner stops cleanly at the region end.

**Single-box legality vs detector threshold** (separate contracts):

- Single-box legality is the framing rule set above (well-formed header + span).
- The detector (`detectMp4Candidate`, P5d-1) grades by **length-chain self-consistency over multiple boxes**, mirroring the AAC ADTS detector: `Strong` requires ≥ 3 consecutive well-formed box headers whose spans tile the inspected prefix; `Probable` = 2; `Weak` = 1. There is no single-evidence downgrade clause (T15b lesson). Exact thresholds are P5d-1 calibration targets; the ≥3 Strong rule is the contract.

**Scanner/DSL data contract — span-only (ADTS-isomorphic), no header-value publication**:

- The scanner outputs each box **span** (start offset, end offset, truncation flag) — exactly the record shape `AacAdtsScanner` produces for frames. It does **not** publish `size`/`type`/`largesize` values.
- The runner maps the span to a source sub-view; the DSL `Box` struct **re-reads** `size`/`type`/`largesize` from the source within the span (identical to how `AdtsHeader` fields are re-read from the ADTS frame span).
- The earlier P5c statement "the scanner publishes size/type/largesize as element values" is **withdrawn** (a value-passing design would require a new typed scan/value schema in IR and VM — see Rejected Alternatives 7).

After P5d-1/P5d-2, `sequence<Box> boxes = scan(mp4_box);` (with `@index(progressive)`) becomes expressible.

### D2: Nested Drilling — `@container` Annotation with a Locked Contract

Today the entire language offers one hierarchy mechanism: top-level `sequence` + `payload<rbsp>` dispatch (fact 6; probes P2a–P2d). Container boxes (`moov`/`trak`/`mdia`/`minf`/`stbl`) need *enumerating children over a byte region*. The chosen mechanism is `@container` + runner re-entry.

**Locked syntax** (post-position on a lazy `bytes` region, one argument, a struct identifier):

```
@lazy(payload_bytes) bytes payload @container(ChildStruct);
```

- **Host**: `DslAnnotationTarget::LazyRegion` only. A `@container` on any other host is rejected.
- **Argument contract**: exactly one argument; the token must parse as `DslAnnotationValueKind::Identifier` (generic annotation-argument parsing classifies identifier tokens at `dsl.cpp:801`; the `@index(progressive)` consumer at `dsl.cpp:3577-3580` is the existing end-to-end precedent that identifier-kind arguments parse and validate — probe P7d carries `@index(progressive)` and is `Rule OK`). The identifier must name a declared struct.
- **Diagnostics** (P5d-2, using existing `DslDiagnosticCode` values; exact message text to be locked by P5d-2 tests):
  - unregistered name (today): `Unknown annotation '@container'` (`dsl.cpp:1880`);
  - host not LazyRegion: not-allowed message (generic `@%1 is not supported on this declaration`, `dsl.cpp:1905-1907`);
  - arity ≠ 1: `InvalidAnnotation`;
  - argument kind ≠ Identifier: `InvalidAnnotation`;
  - target struct not declared: `UnknownReference` (mirrors the sequence element-type lookup, `dsl.cpp:3556-3564`).

**Parser AST**: the generic `DslAnnotation { name = "container", arguments = [{ kind = Identifier, text = structName }] }` produced by the annotation parser (`dsl.cpp:782-810`); probe P8c confirms the name gate fires and the argument parses as Identifier when registered.

**Typed IR**: `DslLazyRegion` gains `std::optional<quint32> containerChildStructIndex` (index into `program.structs`). Compile-time validation: the struct exists; it is a decodable struct (has fields); the region's byte-count expression remains source-anchored under the existing lazy validation.

**Runner read**: when materializing a lazy region carrying `containerChildStructIndex`, the runner re-enters the `Mp4Box` scanner over the region's byte span `[anchor_start, anchor_end)` and materializes child nodes using the named struct as the element type, attached **under the container's lazy node**. The container node stays the lazy node (children replace the opaque byte view; the payload is never eagerly materialized as a byte blob).

**Child mapping boundaries**: children tile `[anchor_start, anchor_end)` exactly. A truncated child at the region end gets `TruncatedSource` on that child node; sibling children are unaffected.

**Recursion/cycle strategy**: re-entry is rule-declared per level — only regions annotated `@container` re-enter, and regions are disjoint shrinking byte spans, so unbounded self-recursion is impossible by construction. The runner additionally enforces a maximum nesting depth (design target 16) and the global node budget (`defaultMaximumMaterializedNodes() = 100'000`, `dsl_vm.h:36-38`) across all levels; exceeding either yields `ResourceLimit` on the offending region.

**Cancellation, truncation, child-error propagation**: child materialization runs under the same batch/cancel/budget contracts as the top-level scan; per-child diagnostics attach to child nodes; a failed or truncated child does not abort siblings; there is no global rollback.

**Core/runner neutrality**: the child struct is referenced by IR index only; **no `moov`/`trak`/`mdia`/`minf`/`stbl` (or any) FourCC literal appears in core or runner code**.

### D3: Type Tag Matching — `bits<32>` + `@equals(0x...)`, No New Value Type

Fact 10 holds: `bits<32> box_type @equals(0x66747970)` compiles; hex integer literals are admitted. Common FourCC constants (big-endian ASCII packed into a 32-bit literal) referenced by P5d+ rules:

| FourCC | Value | FourCC | Value | FourCC | Value | FourCC | Value |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `ftyp` | `0x66747970` | `moov` | `0x6D6F6F76` | `mdat` | `0x6D646174` | `free` | `0x66726565` |
| `mvhd` | `0x6D766864` | `trak` | `0x7472616B` | `tkhd` | `0x746B6864` | `edts` | `0x65647473` |
| `elst` | `0x656C7374` | `mdia` | `0x6D646961` | `mdhd` | `0x6D646864` | `hdlr` | `0x68646C72` |
| `minf` | `0x6D696E66` | `vmhd` | `0x766D6864` | `smhd` | `0x736D6864` | `dinf` | `0x64696E66` |
| `dref` | `0x64726566` | `stbl` | `0x7374626C` | `stsd` | `0x73747364` | `stts` | `0x73747473` |
| `stss` | `0x73747373` | `stsc` | `0x73747363` | `stsz` | `0x7374737A` | `stco` | `0x7374636F` |
| `avc1` | `0x61766331` | `avcC` | `0x61766343` | `mp4a` | `0x6D703461` | `esds` | `0x65736473` |
| `pasp` | `0x70617370` | `btrt` | `0x62747274` | `mvex` | `0x6D766578` | `trex` | `0x74726578` |
| `moof` | `0x6D6F6F66` | `mfhd` | `0x6D666864` | `traf` | `0x74726166` | `tfhd` | `0x74666864` |
| `trun` | `0x7472756E` | `udta` | `0x75647461` | `meta` | `0x6D657461` | `ilst` | `0x696C7374` |

**`uuid` extended-type boxes** (corrected from the initial P5c draft): `bits<128>` is not expressible (probe P8b, `dsl.cpp:1038`). The locked form is:

```
bits<32> type @equals(0x75756964);
bits<64> usertype_hi;
bits<64> usertype_lo;
```

Probe P8a confirms this compiles (`Rule OK`). Contract: v0.1 **transparently preserves** the 128-bit value as two `bits<64>` fields (with the D1 payload-anchor shift of +16); whole-UUID literal equality is **not supported** (no string-literal tokenizer, no bytes value type, fact 10). This closes the open question that ADR-0096 D1 §5 left to ADR-0097.

### D4: Size Branches — Per-Branch `computed` + `@lazy`; the `size == 0` Increment Is a New `available_bytes()` Builtin

- **`size == 1` / `else`**: the only working shape is the per-branch form (fact 3), which ADR-0096 D1 already carries: `if (size == 1) { bits<64> largesize; computed<u64> large_payload = largesize - 16; @lazy(large_payload) bytes large_data; } else { computed<u64> payload = size - 8; @lazy(payload) bytes data; }`. This ADR re-confirms it as the only working shape.
- **`size == 0`**: genuinely inexpressible today (fact 4): `compressed_payload` is restricted to the final top-level item (`dsl.cpp:1589`) so it cannot appear inside an `if` branch, and no remaining-bytes query exists.

**Locked contract for the increment — `available_bytes()` builtin** (P5d-2):

- Semantics: returns the number of **bytes remaining in the current source region from the current read position** (byte-granular; value-returning; usable in any `computed` expression, mirroring `more_rbsp_data()`).
- With it, `size == 0` is expressible as:

```
if (size == 0) {
    computed<u64> payload_bytes = available_bytes();
    @lazy(payload_bytes) bytes payload;
}
```

- The scanner performs the `size == 0` framing (span to EOF, terminal box) per D1; the builtin only lets the rule size its lazy payload region.
- Rejected alternative: scanner-computed remaining-length pass-down (see Rejected Alternatives 8).

### D5: `@target_format` — Full Contract

**Locked syntax** (post-position on a lazy `bytes` region, one string argument):

```
@lazy(payload_bytes) bytes payload @target_format("video/mp4");
```

- **Host**: `DslAnnotationTarget::LazyRegion` only.
- **Argument contract**: exactly one argument; the token must parse as `DslAnnotationValueKind::String` (string-literal parsing at `dsl.cpp:798`); the string must be non-empty.
- **Registry change** (P5d-2): `knownAnnotations` at `dsl.cpp:1834-1866`; the reserved entry `{u"target_format", 0U}` (`dsl.cpp:1865`) becomes `{u"target_format", static_cast<quint32>(DslAnnotationTarget::LazyRegion)}`; the LazyRegion whitelist message (`Lazy byte regions accept only @description and @spec`, `dsl.cpp:1895`) is extended to admit `@target_format` so the P5b gate does not self-block P5h rules.
- **Diagnostics** (P5d-2): arity ≠ 1 or argument kind ≠ String or empty string → `InvalidAnnotation`; host ≠ LazyRegion → not-allowed message (`@%1 is not supported on this declaration`, `dsl.cpp:1905-1907`).
- **Typed IR**: `DslLazyRegion` gains `std::optional<QString> targetFormat`, validated at compile time.
- **Propagation**: the runner stores `targetFormat` in the materialized lazy node's field metadata (an analysis-tree node metadata entry keyed `targetFormat`); the analysis-cache payload envelope serializes node metadata so the value survives cache restore without re-running the rule.
- **Session/cache retention**: a session restored from cache retains the navigation metadata (`analysis_cache_payload` node-metadata serialization).
- **AnalysisSession/UI consumption** (P5i contract): a UI action on an `avcC`/`esds` lazy region reads `targetFormat` from node metadata, resolves the target rule entry via `RulePackageCatalog::resolve` (`rule_catalog.h:52`), and opens the target rule's analysis view with the region's source span mapped back to the container field (bidirectional navigation).
- **P5d implementation points**: `dsl.cpp` registry + whitelist message + argument validation; `dsl_ir.cpp` `DslLazyRegion::targetFormat`; `dsl_vm.cpp` materialize-time metadata; `analysis_model.h` node metadata + `analysis_cache_payload` serialization.
- **P5d test matrix**: positive — lazy region `@target_format("video/mp4")` compiles and IR retains the string; negative — arity 0, arity 2, integer argument, bit-field host, empty string (each with the expected `InvalidAnnotation` code).

### D6: Sample Table Windowing — Default Contract with an Explicit Capability Slice

- The compile-time bound stays `maximumExpandedFieldsPerStructure = 99'999` per struct (`dsl_ir.cpp:13`); expansion past it is a compile error (fact 5; `repeat(count, 99999)` → `Bounded repeat expansion exceeds the structure materialization limit`).
- **Narrowed requirement**: the *product* must support potentially huge sample tables (hundreds of thousands of entries), so the **default** rule shape is the lazy-region windowed form (fact 15): `computed<u64> table_bytes = entry_count * 4; @lazy(table_bytes) bytes entry_table;`. Small tables whose `entry_count` is comfortably under the per-struct bound **may** use bounded `repeat` expansion; windowing is not a per-table compiler mandate.
- **Capability slice**: the generic lazy-region **window decoder** — decode a bounded window of entries from a lazy byte region given `entry_size`/`entry_count`, page forward/back, and recover each entry's absolute source coordinates as `container_anchor + index * entry_size` — is a **runner/session capability**, delivered as a **P5d-3 subitem** (or a standalone P5d-4 if P5d-3 exceeds scope). P5g rules consume it; rules do not reimplement windowing.
- **UI paging contract**: fixed page size; forward/back navigation; window invalidation when the underlying session changes; a single page never exceeds the node budget (`defaultMaximumMaterializedNodes() = 100'000`, `dsl_vm.h:36-38`).

### D7: Fragmented MP4 (`moof`) — In-Rule `unsupported` as a Normative P5d Contract

- **Compile evidence (P5c, verified)**: the shape `bits<32> size; bits<32> type; unsupported("fragmented MP4 (moof/traf) is outside the v0.1 subset") at type;` compiles — probe P7d, `Rule OK`. P7d proves compile-ability only.
- **Runtime behaviors below are P5d runner normative contracts and must-test behaviors, not P5c-verified facts**:
  1. the `moof` box node is materialized with `MaterializationState::Unsupported`, `DiagnosticCode::UnsupportedSyntax`, `DiagnosticSeverity::Warning`, anchored at the `type` field;
  2. prior top-level boxes (`ftyp`, `moov`, ...) remain materialized;
  3. top-level scanning **continues**: a box following `moof` in the same region (e.g., the `mdat` that follows `moof` in the fragmented fixture) is still materialized.
- **P5d tests must assert all three**; the test vector is the fragmented fixture `/tmp/p5c_frag.mp4` whose `ffprobe -v trace` excerpt shows `moof` → `mfhd`/`traf` → `tfhd`/`trun`, then a subsequent top-level `mdat` (see Verification Matrix).
- The three `unsupported` prohibitions do not block the container scenario (probes P7a/P7b/P7c, `dsl_ir.cpp:2741/2748/2771`): the `moof` check sits at the top level of the box struct (not repeat-local); the reason is always non-empty; the anchor is the source-backed `bits<32> type` field, never a `computed` field.
- Rejected alternative: detector-grade whole-file rejection (see Rejected Alternatives 9).

---

## Rejected Alternatives

1. **Struct-typed fields** (`BoxHeader header;`) for nesting — rejected: the parser rejects struct types as field types today (probe P2a), and `@container` (D2) achieves nesting without new field types.
2. **Nested `sequence` declarations** — rejected: requires new grammar, IR lowering, and VM sequencing semantics; conflicts with the top-level-only sequence contract (probe P2b); `@container` is strictly smaller.
3. **New `payload<box>` view kind for nesting** — rejected for v0.1: the dispatch view selects one struct per value while containers enumerate many children; it does not solve nesting (probe P2c). May return later as a top-level dispatch ergonomic refinement. The view-kind closed set stays `{rbsp}` for now (`dsl.cpp:3669`, `dsl_ir.cpp:3717`).
4. **`bits<128>` for the `uuid` usertype** — rejected: not expressible (`Bit field width must be in the range 1..64`, `dsl.cpp:1038`, probe P8b); the locked form is two `bits<64>` fields (probe P8a).
5. **Whole-UUID literal equality** — rejected for v0.1: no string-literal tokenizer and no bytes value type (fact 10); v0.1 transparently preserves the 128-bit value only.
6. **Detector-grade fragmented-MP4 rejection** — rejected (D7): loses the decoded prefix and subsequent-box scanning and injects FourCC logic into core detection.
7. **Scanner header-value publication** (scanner emits `size`/`type`/`largesize` as element values) — rejected: the ADTS-isomorphic span-only contract (D1) is strictly smaller and matches the existing runner record mapping; value publication would require a new typed scan/value schema in IR and VM with no demonstrated benefit.
8. **Scanner-computed remaining-length pass-down for `size == 0`** — rejected in favor of the `available_bytes()` builtin (D4): the builtin is format-neutral, reusable, and avoids context plumbing for a corner case.
9. **A new `bytes` value type for FourCC** — already rejected by fact 10: `bits<32>` + `@equals(0x...)` suffices.

---

## Consequences

### Positive

- The ISOBMFF box tree becomes fully expressible after P5d: top-level enumeration with a locked framing contract (D1), container drilling (D2), type dispatch (D3), size arithmetic including the `size == 0` EOF case (D4), a default windowing contract with an explicit runner capability slice (D6), and a normative fragmented-MP4 contract (D7).
- `@target_format` (D5) provides the cross-format navigation metadata that P5i consumes, with a locked propagation and cache-retention contract.
- Core stays format-neutral: the scanner knows framing only (D1); the runner's re-entry is generic (D2); no FourCC literal enters `src/` core paths.
- Every capability increment in this ADR has a directly codeable and testable contract; P5d slices are independently closable per the T4a/T4b precedent.

### Negative

- Four new capability increments must ship in P5d slices: (1) `DslScannerKind::Mp4Box` scanner + detector, (2) `@container` annotation + runner re-entry, (3) `available_bytes()` builtin, (4) `@target_format` registration + LazyRegion whitelist relaxation — plus the window decoder (P5d-3 subitem / P5d-4).
- The `payload<rbsp>`-only restriction remains; top-level dispatch must use `if`/`else` on `type` until/unless a dispatch view kind is added later.
- `size == 0` boxes are terminal by construction (extend to EOF), so a `size == 0` box can never be followed by another top-level box in the same region; rules must reflect that.

---

## Verification Matrix and Evidence

All 14 probes were executed this round with the working-tree tool binary `build/dev/tools/svtool/svtool` (`svtool 0.1.0 (DSL 0.1)`); scratch sources live outside the repository under the session scratch directory. The matrix below lists each probe's command and result; **full probe sources, full executable commands, and raw outputs are reproduced in the P5c-R task report**.

| # | Probe | Command | Result |
| :--- | :--- | :--- | :--- |
| P1 | `scan(mp4_box)` | `svtool rule check scratch/p5c_p1_scan_mp4_box.svfmt` | `error: Only h264_start_code and adts_frame are supported` (`dsl.cpp:3568`) |
| P2a | struct-typed field | `svtool rule check scratch/p5c_p2a_struct_typed_field.svfmt` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` |
| P2b | `sequence` inside struct | `svtool rule check scratch/p5c_p2b_nested_sequence.svfmt` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` |
| P2c | `payload<mp4>` view kind | `svtool rule check scratch/p5c_p2c_payload_mp4_kind.svfmt` | `error: The only accepted payload view kind is rbsp` |
| P2d | `@container` | `svtool rule check scratch/p5c_p2d_container_annotation.svfmt` | `error: Unknown annotation '@container'` |
| P5a | `@target_format` post-position lazy | `svtool rule check scratch/p5c_p5a_target_format_lazy.svfmt` | `error: Lazy byte regions accept only @description and @spec` (`dsl.cpp:1895`) |
| P5b | `@target_format` pre-position | `svtool rule check scratch/p5c_p5b_target_format_preposition.svfmt` | `error: Expected bytes after @lazy(...)` (`dsl.cpp:1112`) |
| P7a | `unsupported("")` | `svtool rule check scratch/p5c_p7a_unsupported_empty_reason.svfmt` | `error: Unsupported statements require a non-empty reason` (`dsl_ir.cpp:2748`) |
| P7b | `unsupported` on computed anchor | `svtool rule check scratch/p5c_p7b_unsupported_computed_anchor.svfmt` | `error: Unsupported anchors require a source-backed scalar field` (`dsl_ir.cpp:2771`) |
| P7c | `unsupported` inside `repeat` | `svtool rule check scratch/p5c_p7c_unsupported_repeat_local.svfmt` | `error: Unsupported statements cannot be repeat-local items` (`dsl_ir.cpp:2741`) |
| P7d | `unsupported` after box header (moof shape) | `svtool rule check scratch/p5c_p7d_unsupported_positive.svfmt` | `Rule OK` |
| P8a | uuid marker, two `bits<64>` usertype | `svtool rule check scratch/p5c_p8_uuid_marker.svfmt` | `Rule OK` |
| P8b | `bits<128>` field | `svtool rule check scratch/p5c_p8_bits128.svfmt` | `error: Bit field width must be in the range 1..64` (`dsl.cpp:1038`) |
| P8c | `@container(Child)` post-position | `svtool rule check scratch/p5c_p8_container_annotation.svfmt` | `error: Unknown annotation '@container'` (name gate; `Child` parses as Identifier, `dsl.cpp:801`) |

**Fixture generation (full commands, executed this round; outputs are relevant excerpts via `grep`, not raw full output):**

- Regular fixture:
  `ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=duration=0.2:size=64x48:rate=10 -f lavfi -i sine=frequency=440:duration=0.2 -c:v libx264 -preset ultrafast -c:a aac -y /tmp/p5c_fixture.mp4`
  → 6,513-byte file (`ls -la /tmp/p5c_fixture.mp4` → `-rw-r--r--@ 1 yun wheel 6513`).
- Fragmented fixture:
  `ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=duration=0.3:size=64x48:rate=10 -c:v libx264 -preset ultrafast -movflags frag_keyframe+empty_moov -y /tmp/p5c_frag.mp4`
  → 4,505-byte file.

**Box-level ground truth (`ffprobe -v trace`, relevant excerpt via `grep`)** — regular fixture, covering the nodes this ADR claims (including `smhd` and both tracks' `btrt`):

```
type:'ftyp' parent:'root' sz: 32 8 6513
type:'free' parent:'root' sz: 8 40 6513
type:'mdat' parent:'root' sz: 5012 48 6513
type:'moov' parent:'root' sz: 1461 5060 6513
type:'mvhd' parent:'moov' sz: 108 8 1453
type:'trak' parent:'moov' sz: 606 116 1453        (video track)
type:'tkhd' parent:'trak' sz: 92 8 598
type:'edts' parent:'trak' sz: 36 100 598
type:'elst' parent:'edts' sz: 28 8 28
type:'mdia' parent:'trak' sz: 470 136 598
type:'mdhd' parent:'mdia' sz: 32 8 462
type:'hdlr' parent:'mdia' sz: 45 40 462
type:'minf' parent:'mdia' sz: 385 85 462
type:'vmhd' parent:'minf' sz: 20 8 377
type:'dinf' parent:'minf' sz: 36 28 377
type:'dref' parent:'dinf' sz: 28 8 28
type:'stbl' parent:'minf' sz: 321 64 377
type:'stsd' parent:'stbl' sz: 189 8 313
size=173 4CC=avc1 codec_type=0
type:'avcC' parent:'stsd' sz: 51 8 87
type:'pasp' parent:'stsd' sz: 16 59 87
type:'btrt' parent:'stsd' sz: 20 75 87            (video btrt)
type:'stts' parent:'stbl' sz: 24 197 313
type:'stss' parent:'stbl' sz: 20 221 313
type:'stsc' parent:'stbl' sz: 28 241 313
type:'stsz' parent:'stbl' sz: 28 269 313
type:'stco' parent:'stbl' sz: 24 297 313
type:'trak' parent:'moov' sz: 641 722 1453        (audio track)
type:'stsd' parent:'stbl' sz: 126 8 352
size=110 4CC=mp4a codec_type=1
type:'esds' parent:'stsd' sz: 54 8 74
type:'btrt' parent:'stsd' sz: 20 62 74            (audio btrt)
type:'smhd' parent:'minf' sz: 16 8 412            (audio sound media header)
```

**Fragmented fixture (`ffprobe -v trace`, relevant excerpt via `grep`)** — covering `mvex`/`trex` and the `moof` chain with the subsequent `mdat`:

```
type:'mvex' parent:'moov' sz: 40 610 740
type:'trex' parent:'mvex' sz: 32 8 32
type:'moof' parent:'root' sz: 124 792 4505
type:'mfhd' parent:'moof' sz: 16 8 116
type:'traf' parent:'moof' sz: 100 24 116
type:'tfhd' parent:'traf' sz: 36 8 92
type:'trun' parent:'traf' sz: 36 64 92
type:'mdat' parent:'root' sz: 3530 916 4505       (follows moof; D7 continuation target)
```

**Sample-level ground truth (`ffprobe -show_packets`, video stream, relevant excerpt):**

```
$ ffprobe -v error -select_streams v:0 -show_packets -show_entries packet=pos,pts_time,duration_time,flags -of csv=p=0 /tmp/p5c_fixture.mp4
0.000000,0.100000,306,K__      (frame 1: pts 0.0s, duration 0.1s, absolute byte pos 306, keyframe)
0.100000,0.100000,3282,___     (frame 2: pts 0.1s, pos 3282, non-keyframe)
```

Toolchain conclusion: `ffprobe` 8.1 and `ffmpeg` are available; `MP4Box` is not installed (`which MP4Box mp4box` → not found). `ffmpeg` + `ffprobe -v trace` (box types/sizes/offsets/parents) plus `ffprobe -show_packets` (sample offsets/timestamps/keyframes) provide generation and ground-truth verification without any hand-computed bit offsets, satisfying the "no manual bit positions" convention for P5d+ fixtures.

---

## References

- ADR-0096: MP4/ISOBMFF Container Architecture, Box Traversal, Cross-Layer Navigation, and Sample Indexing Boundaries (its D1 branch-local `computed` + `@lazy` form is re-confirmed by this ADR; its `size == 0` and `uuid` open gaps are resolved here).
- ADR-0098: Unrecognized Annotation Compiler Gate and Explicit Unsupported Syntax (annotation registry `knownAnnotations`, `dsl.cpp:1834-1866`, and the `unsupported` statement contracts).
- ADR-0040: Non-Fatal Syntax Warnings and Range Annotations (fatal/non-fatal dichotomy applied in D7).
- ISO/IEC 14496-12:2015 (ISOBMFF box structure and `size`/`largesize`/`size == 0` semantics; `uuid` 16-byte `usertype`).
- Task P5c/P5c-R definition and plan facts 10–16, `Sub-Agent分步开发指导计划.md`.
