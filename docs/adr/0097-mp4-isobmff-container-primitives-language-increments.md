# ADR-0097: MP4/ISOBMFF Container Primitive Expressibility and Language Increments

- **Status**: Proposed
- **Date**: 2026-08-17
- **Authors**: StreamView Contributors

---

## Context

Phase 5 requires non-fragmented MP4/ISOBMFF container parsing (`video/mp4`, `audio/mp4`, ISO/IEC 14496-12 / 14496-14 / 14496-15). ADR-0096 (P5a) fixed the architectural boundaries — D1 box traversal and `mdat` lazy encapsulation, D2 cross-layer navigation (`avcC`/`esds` to H.264/AAC), D3 sample-table windowing — but its D1 DSL fragment referenced language constructs that do not exist. This ADR (Task P5c) probes what the current DSL/runtime can and cannot express for ISOBMFF container structure, and decides the minimal language and core increments. It authors **no rules**: all rule assets are deferred to P5d+ per the "enumeration/drilling mechanism unresolved ⇒ no MP4 rule assets" gate.

The six container facts (plan facts 10–16, measured by the main agent on 2026-08-16 with `svtool rule check`) are incorporated without re-probing and re-confirmed where a probe was run this round:

1. **FourCC matching is expressible today**: `bits<32> box_type @equals(0x66747970)` compiles (`Rule OK`). No new `bytes` value type is needed.
2. **`bits<64>` is allowed** — `largesize` is directly readable.
3. **`size == 1` / `else` payload sizes are expressible**, but only with `computed` and `@lazy` placed **inside each `if`/`else` branch** (a single cross-branch `computed` fails with `Computed dependency is not guaranteed on the current branch`).
4. **`size == 0` (extend to EOF) is the only true language gap**: `compressed_payload` is the sole "consume the rest" terminal but is constrained to the final top-level item (`error: compressed_payload must occur once as the final top-level item`, `dsl.cpp:1589`), and no `available_bytes` builtin exists.
5. **`maximumExpandedFieldsPerStructure = 99'999` per struct** (`dsl_ir.cpp:13`) makes sample-table windowing a hard compile-time requirement, not an optimization.
6. **The only hierarchy mechanism today is a top-level `sequence` plus a single `payload<rbsp>` dispatch** (the H.264 shape). Nested box enumeration is the hard blocker this ADR resolves.

Probe evidence collected this round (scratch, `svtool rule check`):

| Probe | Result (verbatim error / status) | Live location |
| :--- | :--- | :--- |
| `scan(mp4_box)` | `error: Only h264_start_code and adts_frame are supported` | parser `dsl.cpp:3568`, IR `dsl_ir.cpp:3679` |
| `BoxHeader header;` (struct-typed field) | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` | field-type parser |
| `sequence<Child> children = scan(...)` inside a struct | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` | sequence is top-level only |
| `payload<mp4> ...` view kind | `error: The only accepted payload view kind is rbsp` | parser `dsl.cpp:3669`, IR `dsl_ir.cpp:3717` |
| `@container` annotation | `error: Unknown annotation '@container'` | `dsl.cpp:1880` |
| `@lazy(4) bytes payload @target_format("video/mp4")` | `error: Lazy byte regions accept only @description and @spec` | `dsl.cpp:1895` |
| `@lazy(4) @target_format(...) bytes payload` (pre-position) | `error: Expected bytes after @lazy(...)` | `dsl.cpp:1112` |
| `unsupported("") at type;` | `error: Unsupported statements require a non-empty reason` | `dsl_ir.cpp:2748` |
| `unsupported(...) at <computed field>` | `error: Unsupported anchors require a source-backed scalar field` | `dsl_ir.cpp:2771` |
| `unsupported(...) at type;` inside `repeat` | `error: Unsupported statements cannot be repeat-local items` | `dsl_ir.cpp:2741` |
| `unsupported("fragmented MP4 (moof/traf) is outside the v0.1 subset") at type;` after box header fields | `Rule OK` | — |

---

## Decision

### D1: Top-Level Box Enumeration — New `DslScannerKind::Mp4Box`

The scanner-kind closed set `{H264StartCode, AacAdtsFrame}` (`dsl_ir.h:219-221`) is extended with `Mp4Box`. The new scanner is **framing-only**, symmetric to `AacAdtsScanner` (which recognizes the `0xFFF` syncword and the `aac_frame_length` chain but no profile semantics):

- At each box start it reads `u32 size` and the 4-byte `type`; if `size == 1` it additionally reads `u64 largesize`; if `size == 0` the box span extends to EOF and the box is terminal.
- The box span is the pure length-prefix advance: `size` (or `largesize`), or `remaining bytes` for `size == 0`.
- The scanner **must not recognize any specific FourCC**. "Which FourCC means what" is format semantics and stays in the DSL rule (`bits<32> type @equals(0x...)` dispatch). The scanner knows the *frame*, never the *meaning* — the same stance as H.264 start codes and ADTS length links.
- Candidate detection: a well-formed box header (`size >= 8`, or `size == 0`, and the span fits the remaining source) followed by at least one more well-formed header (or EOF) constitutes a candidate, mirroring the ADTS "≥N-frame length chain" stance; exact thresholds are calibrated in P5d.

After P5d, `sequence<Box> boxes = scan(mp4_box);` (with `@index(progressive)`) becomes expressible. The scanner publishes `size`/`type`/`largesize` as element values so the rule's `Box` struct declares and materializes them (same record contract as ADTS header fields).

### D2: Nested Drilling — `@container` Annotation + Runner Re-Entry (minimal mechanism)

Today the entire language offers one hierarchy mechanism: top-level `sequence` + `payload<rbsp>` dispatch (fact 6; probes above). Container boxes (`moov`/`trak`/`mdia`/`minf`/`stbl`) need *enumerating children over a byte region*. Three candidates:

- **A — `@container` annotation + runner re-entry**: the rule marks a lazy `bytes` region with `@container`; the runner re-enters the `Mp4Box` scanner over that byte span and materializes child box nodes under the container node.
- **B — nested `sequence` declarations** (`sequence<Child> children = scan(...)` inside a struct body): rejected by the field-type parser today (probe above); requires new grammar, IR, and VM sequencing semantics and breaks the "sequence is top-level only" contract — the heaviest change.
- **C — new `payload<box>` view kind** plus relaxing "payload must attach to a declared top-level sequence": the payload dispatch selects *one* struct per switch value; containers need *enumeration of many* children, so a dispatch view alone does not solve nesting. It would only improve top-level dispatch ergonomics.

**Decision: A.** Division of responsibility:

- **DSL**: "which regions are containers" is format semantics — the rule annotates its lazy payload region with `@container` and names the child box struct. FourCC dispatch and per-box payload layouts remain in the rule.
- **Core/runner**: the re-entrant box scan (byte span → `Mp4Box` enumeration → child nodes under the container) is format-neutral and contains **zero FourCC literals**.

C remains a possible future ergonomic refinement for top-level dispatch; it is not required for v0.1.

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

**`uuid` extended-type boxes**: the `uuid` marker is expressible as `bits<32> type @equals(0x75756964)` followed by an opaque `bits<128> usertype;` field. Byte-array equality against a specific UUID remains inexpressible (no string-literal tokenizer and no bytes value type, fact 10), so `uuid` boxes in v0.1 are declared with an opaque `usertype` or reported via `unsupported`; this closes the open question that ADR-0096 D1 §5 left to ADR-0097.

### D4: Size Branches — Per-Branch `computed` + `@lazy`; the `size == 0` Increment Is a New `available_bytes()` Builtin

- **`size == 1` / `else`**: the only working shape is the per-branch form (fact 12), which ADR-0096 D1 already carries: `if (size == 1) { bits<64> largesize; computed<u64> large_payload = largesize - 16; @lazy(large_payload) bytes large_data; } else { computed<u64> payload = size - 8; @lazy(payload) bytes data; }`. This ADR re-confirms it as the only working shape.
- **`size == 0`**: genuinely inexpressible today (fact 13): `compressed_payload` is restricted to the final top-level item (`dsl.cpp:1589`) so it cannot appear inside an `if` branch, and no remaining-bytes query exists.

Two candidates for the minimal increment:

- **A — scanner-computed remaining length passed down**: the scanner resolves `size == 0` to the actual remaining length and hands it to the rule through a header/context value flow. Rejected: couples scanner internals to the DSL value flow, is only meaningful at the exact box boundary, and needs new context plumbing for one corner case.
- **B — new `available_bytes()` builtin**: a byte-granular "remaining bytes in the current source region from the current read position" query usable in any `computed` expression (mirroring `more_rbsp_data()` but byte-oriented and value-returning).

**Decision: B.** It is format-neutral, minimal, reusable, and makes `size == 0` expressible as:

```
if (size == 0) {
    computed<u64> payload_bytes = available_bytes();
    @lazy(payload_bytes) bytes payload;
}
```

The scanner still performs the `size == 0` framing (span to EOF, terminal box) per D1; the builtin only lets the rule size its lazy payload region. P5d implements the builtin in the expression language and VM.

### D5: `@target_format` — Central Registry Integration Design

The annotation registry is `knownAnnotations` in `dsl.cpp:1836-1865`; `@target_format` is currently reserved with an empty host set (`{u"target_format", 0U}` at `dsl.cpp:1865`, comment "Reserved for Task P5h" at `dsl.cpp:1864`). Design for P5d-2:

- Register `{u"target_format", DslAnnotationTarget::LazyRegion}` so the generic whitelist admits it on lazy byte regions. The LazyRegion special-case message (`Lazy byte regions accept only @description and @spec`, `dsl.cpp:1895`) must be extended to admit `@target_format` so the host whitelist and the message stay consistent (the P5b gate must not be a self-block: P5h rules use `@target_format`).
- **Position is post-position only** (after `bytes name`): inserting an annotation between `@lazy(...)` and `bytes` is rejected (`Expected bytes after @lazy(...)`, `dsl.cpp:1112`; probe above). Usage shape:

```
@lazy(payload_bytes) bytes payload @target_format("video/mp4");
```

- Semantics: the annotation labels the lazy region with the target format identifier consumed by `AnalysisSession`/UI for cross-format navigation (P5h: `avcC`/`esds` regions → `RulePackageCatalog::resolve` target entry → coordinate-mapped view).
- This ADR designs only; the registry and whitelist changes land in P5d-2 per the plan's capability-slice ordering (the `dsl.cpp:1864` comment names P5h because P5h is the *consumption* task).

### D6: Sample Table Windowing — Mandatory, with a Paging/Coordinate Contract

Windowing is a hard requirement, not an optimization: `maximumExpandedFieldsPerStructure = 99'999` per struct (`dsl_ir.cpp:13`) turns table expansion past the limit into a compile error (fact 14: `repeat(count, 99999)` → `Bounded repeat expansion exceeds the structure materialization limit`; two 60,000-entry tables in one struct fail; splitting across structs works). The default shape is the lazy-region form (fact 15):

```
computed<u64> table_bytes = entry_count * 4;
@lazy(table_bytes) bytes entry_table;
```

Windowed-read contract (design):

- **Materialization**: table metadata (`entry_count`, table configuration) is fully materialized; entry arrays are lazy byte regions, never expanded fields.
- **Window decoding**: a bounded window (page) of entries is decoded on demand by the runner/session on user request; no eager full-table decoding.
- **Source-coordinate remapping**: each window entry's logical range is computed as the container anchor plus `index * entry_size`; absolute source bits are recoverable and shown by the UI as source offsets. Coordinates are derived from the lazy region's anchor, never hand-computed.
- **UI paging contract**: fixed page size; forward/back navigation; window invalidation when the underlying session changes; a page is never materialized beyond the node budget (`defaultMaximumMaterializedNodes() = 100'000`, `dsl_vm.h:36-38`).

### D7: Fragmented MP4 (`moof`) — In-Rule `unsupported` After the Box Header

Two mechanisms:

- **A — in-rule `unsupported`**: the rule declares `moof`/`mfhd`/`traf` box structs; after reading `size`/`type` (optionally `mfhd`), it emits `unsupported("fragmented MP4 (moof/traf) is outside the v0.1 subset") at type;`. Consequences: the already-decoded prefix (`ftyp`, `moov`, ...) stays materialized; the `moof` box node remains in the tree with `MaterializationState::Unsupported` and `DiagnosticCode::UnsupportedSyntax` (Warning) anchored at `type`; top-level box scanning **continues** to subsequent boxes (e.g., the `mdat` after a `moof`). The probe (below) shows this shape compiles (`Rule OK`).
- **B — detector-grade rejection**: the detector classifies the whole source as unsupported before tree construction; no box nodes; one source-level error; and it forces FourCC sniffing (`moof`) into the detector — format semantics in core detection.

**Decision: A.** It keeps the decoded prefix and per-box granularity, continues subsequent top-level scanning, keeps the detector format-agnostic, and matches ADR-0040's non-fatal philosophy (the stream is valid; only the declared subset is exceeded).

The three `unsupported` prohibitions do not block the container scenario (verified by probes against `dsl_ir.cpp:2741/2748/2771`): (i) not repeat-local — the `moof` check sits at the top level of the box struct; (ii) non-empty reason — a real reason string is always supplied; (iii) source-backed scalar anchor — the anchor is the `bits<32> type` field, never a `computed` field.

---

## Rejected Alternatives

1. **Struct-typed fields** (`BoxHeader header;`) for nesting — rejected: the parser rejects struct types as field types today, and `@container` (D2) achieves nesting without new field types.
2. **Nested `sequence` declarations** — rejected: requires new grammar, IR lowering, and VM sequencing; conflicts with the top-level-only sequence contract; `@container` is strictly smaller.
3. **New `payload<box>` view kind for nesting** — rejected for v0.1: the dispatch view selects one struct per value while containers enumerate many children; it does not solve nesting. May return later as a top-level dispatch ergonomic refinement. The view-kind closed set stays `{rbsp}` for now (`dsl.cpp:3669`, `dsl_ir.cpp:3717`).
4. **Scanner-computed remaining length pass-down for `size == 0`** — rejected in favor of the `available_bytes()` builtin (D4): the builtin is format-neutral, reusable, and avoids context plumbing for a corner case.
5. **Detector-grade fragmented-MP4 rejection** — rejected (D7): loses the decoded prefix and subsequent-box scanning and injects FourCC logic into core detection.
6. **A new `bytes` value type for FourCC** — already rejected by fact 10: `bits<32>` + `@equals(0x...)` suffices; no new value type is introduced.

---

## Consequences

### Positive

- The ISOBMFF box tree becomes fully expressible after P5d: top-level enumeration (D1), container drilling (D2), type dispatch (D3), size arithmetic including the `size == 0` EOF case (D4), and a hard windowing contract for sample tables (D6).
- `@target_format` (D5) provides the cross-format navigation metadata that P5h consumes, without a self-blocking annotation gate.
- Fragmented MP4 is handled non-fatally at the rule level (D7): decoded prefix preserved, per-box diagnostics, continued scanning — consistent with ADR-0040.
- Core stays format-neutral: the scanner knows framing only (D1); the runner's re-entry is generic (D2); no FourCC literal enters `src/` core paths.

### Negative

- Four new capability increments must ship in P5d slices: (1) `DslScannerKind::Mp4Box` + scanner, (2) `@container` annotation + runner re-entry, (3) `available_bytes()` builtin, (4) `@target_format` registration + LazyRegion whitelist relaxation. Each is independently testable per the T4a/T4b precedent.
- The `payload<rbsp>`-only restriction remains; top-level dispatch must use `if`/`else` on `type` until/unless a dispatch view kind is added later.
- `size == 0` boxes are terminal by construction (extend to EOF), so a `size == 0` box can never be followed by another top-level box in the same region; rules must reflect that.

---

## Verification Matrix and Evidence

All probes were executed this round with the working-tree tool binary `build/dev/tools/svtool/svtool` (`svtool 0.1.0 (DSL 0.1)`); scratch sources live outside the repository under the session scratch directory. Full commands and outputs are reproduced in the P5c task report.

| # | Probe | Command | Result |
| :--- | :--- | :--- | :--- |
| P1 | `scan(mp4_box)` top-level enumeration | `svtool rule check p5c_p1_scan_mp4_box.svfmt` | `error: Only h264_start_code and adts_frame are supported` (`dsl.cpp:3568`) |
| P2a | struct-typed field | `svtool rule check p5c_p2a_struct_typed_field.svfmt` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` |
| P2b | `sequence` inside struct | `svtool rule check p5c_p2b_nested_sequence.svfmt` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` |
| P2c | `payload<mp4>` view kind | `svtool rule check p5c_p2c_payload_mp4_kind.svfmt` | `error: The only accepted payload view kind is rbsp` |
| P2d | `@container` annotation | `svtool rule check p5c_p2d_container_annotation.svfmt` | `error: Unknown annotation '@container'` |
| P5a | `@target_format` post-position on lazy region | `svtool rule check p5c_p5a_target_format_lazy.svfmt` | `error: Lazy byte regions accept only @description and @spec` (`dsl.cpp:1895`) |
| P5b | `@target_format` between `@lazy` and `bytes` | `svtool rule check p5c_p5b_target_format_preposition.svfmt` | `error: Expected bytes after @lazy(...)` (`dsl.cpp:1112`) |
| P7a | `unsupported("")` empty reason | `svtool rule check p5c_p7a_unsupported_empty_reason.svfmt` | `error: Unsupported statements require a non-empty reason` (`dsl_ir.cpp:2748`) |
| P7b | `unsupported` on computed anchor | `svtool rule check p5c_p7b_unsupported_computed_anchor.svfmt` | `error: Unsupported anchors require a source-backed scalar field` (`dsl_ir.cpp:2771`) |
| P7c | `unsupported` inside `repeat` | `svtool rule check p5c_p7c_unsupported_repeat_local.svfmt` | `error: Unsupported statements cannot be repeat-local items` (`dsl_ir.cpp:2741`) |
| P7d | `unsupported` after box-header fields (moof shape) | `svtool rule check p5c_p7d_unsupported_positive.svfmt` | `Rule OK` |
| T1 | Fixture generation | `ffmpeg ... -f lavfi -i testsrc=... -c:v libx264 -preset ultrafast -c:a aac -y /tmp/p5c_fixture.mp4` | 6,513-byte regular MP4 produced |
| T2 | Box ground truth | `ffprobe -v trace -i /tmp/p5c_fixture.mp4` | box hierarchy: `ftyp`/`free`/`mdat`/`moov`; `moov`→`mvhd`/`trak`×2; `trak`→`tkhd`/`edts`/`mdia`; `mdia`→`mdhd`/`hdlr`/`minf`; `minf`→`vmhd`/`smhd`/`dinf`/`stbl`; `stbl`→`stsd`(`avc1`+`avcC`/`pasp`/`btrt`, `mp4a`+`esds`/`btrt`)/`stts`/`stss`/`stsc`/`stsz`/`stco` |
| T3 | Fragmented fixture | `ffmpeg ... -movflags frag_keyframe+empty_moov -y /tmp/p5c_frag.mp4` | 4,505-byte fragmented MP4: `moov`(+`mvex`/`trex`) then `moof`→`mfhd`/`traf`→`tfhd`/`trun`, then `mdat` |
| T4 | Sample-level ground truth | `ffprobe -v error -select_streams v:0 -show_packets -show_entries packet=pos,pts_time,duration_time,flags -of csv=p=0 /tmp/p5c_fixture.mp4` | video frame 1: pos 306, pts 0.0, keyframe; frame 2: pos 3282, pts 0.1 |

Toolchain conclusion: `ffprobe` 8.1 and `ffmpeg` are available; `MP4Box` is not installed. `ffmpeg` + `ffprobe -v trace` (box types/sizes/offsets/parents) plus `ffprobe -show_packets` (sample offsets/timestamps/keyframes) provide generation and ground-truth verification without any hand-computed bit offsets, satisfying the "no manual bit positions" convention for P5d+ fixtures.

---

## References

- ADR-0096: MP4/ISOBMFF Container Architecture, Box Traversal, Cross-Layer Navigation, and Sample Indexing Boundaries (its D1 branch-local `computed` + `@lazy` form is re-confirmed by this ADR; its `size == 0` and `uuid` open gaps are resolved here).
- ADR-0098: Unrecognized Annotation Compiler Gate and Explicit Unsupported Syntax (annotation registry and `unsupported` statement contracts).
- ADR-0040: Non-Fatal Syntax Warnings and Range Annotations (fatal/non-fatal dichotomy applied in D7).
- ISO/IEC 14496-12:2015 (ISOBMFF box structure and `size`/`largesize`/`size == 0` semantics).
- Task P5c definition and plan facts 10–16, `Sub-Agent分步开发指导计划.md`.
