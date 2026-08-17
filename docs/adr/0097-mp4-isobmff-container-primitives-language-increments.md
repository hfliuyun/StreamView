# ADR-0097: MP4/ISOBMFF Container Primitive Expressibility and Language Increments

- **Status**: Proposed
- **Date**: 2026-08-17
- **Authors**: StreamView Contributors

---

## Context

Phase 5 requires non-fragmented MP4/ISOBMFF container parsing (`video/mp4`, `audio/mp4`, ISO/IEC 14496-12 / 14496-14 / 14496-15). ADR-0096 (P5a) fixed architectural boundaries — D1 box traversal and `mdat` lazy encapsulation, D2 cross-layer navigation (`avcC`/`esds` to H.264/AAC), D3 sample-table windowing — but its D1 DSL fragment referenced language constructs that do not exist. This ADR (Task P5c, finalized in P5c-R4) probes what the current DSL/runtime can and cannot express for ISOBMFF container structure and fixes the **precise, directly codeable and testable contracts** for every capability increment, each mapped to a real C++ type/function in the working tree. It authors **no rules**: all rule assets are deferred to P5d+ per the "enumeration/drilling mechanism unresolved => no MP4 rule assets" gate.

The seven container facts (plan facts 10-16, measured by the main agent on 2026-08-16 with `svtool rule check`) are incorporated without re-probing and re-confirmed where a probe was run this round:

1. **FourCC matching is expressible today**: `bits<32> box_type @equals(0x66747970)` compiles (`Rule OK`). No new `bytes` value type is needed.
2. **`bits<64>` is allowed** — `largesize` is directly readable; the field-width ceiling is 64 bits (`Bit field width must be in the range 1..64`, `dsl.cpp:1038`).
3. **`size == 1` / `else` payload sizes are expressible**, but only with `computed` and `@lazy` placed **inside each `if`/`else` branch** (a single cross-branch `computed` fails with `Computed dependency is not guaranteed on the current branch`).
4. **`size == 0` (extend to EOF) is the only true language gap**: `compressed_payload` is the sole "consume the rest" terminal but is constrained to the final top-level item (`error: compressed_payload must occur once as the final top-level item`, `dsl.cpp:1589`), and no `available_bytes` builtin exists.
5. **`maximumExpandedFieldsPerStructure = 99'999` per struct** (`dsl_ir.cpp:13`) bounds per-struct field expansion; sample tables above the bound must use lazy-region windowing.
6. **The only hierarchy mechanism today is a top-level `sequence` plus a single `payload<rbsp>` dispatch** (the H.264 shape). Nested box enumeration is the hard blocker this ADR resolves.
7. **`bits<128>` is not expressible** (probe: `error: Bit field width must be in the range 1..64`, `dsl.cpp:1038`); a 128-bit value is declared as two `bits<64>` fields.

All 18 probes were executed with the working-tree tool binary `build/dev/tools/svtool/svtool` (`svtool 0.1.0 (DSL 0.1)`); scratch sources live outside the repository under the session scratch directory. The matrix below lists each probe's command and normalized result:

| Probe | Command | Result (normalized error / status) | Live location |
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
| P8d uuid full size branches (correct wire order) | `svtool rule check scratch/p5c_p8d_uuid_full_branches.svfmt` | `Rule OK` (validates syntax/IR legality of correct wire order) | — |
| P8e uuid `size == 0` gap (`available_bytes()`) | `svtool rule check scratch/p5c_p8e_uuid_size0_gap.svfmt` | `error: Pure function is not declared before this call` (first exposes missing available_bytes builtin) | `dsl_ir.cpp:772` |
| P9a `@window(Entry, entry_count)` post-position | `svtool rule check scratch/p5c_p9a_window_annotation.svfmt` | `error: Unknown annotation '@window'` (name gate) | `dsl.cpp:1880` |
| P9b `@window(123, 456)` non-identifier argument | `svtool rule check scratch/p5c_p9b_window_token_kind.svfmt` | `error: Unknown annotation '@window'` (name gate) | `dsl.cpp:1880` |

---

## Decision

### D1: Top-Level Box Enumeration — New `DslScannerKind::Mp4Box` with a Locked Framing Contract

The scanner-kind closed set `{H264StartCode, AacAdtsFrame}` (`dsl_ir.h:219-221`) is extended with `Mp4Box`. The new scanner is **framing-only** and **must not recognize any specific FourCC**: "which FourCC means what" is format semantics that stays in the DSL rule (`bits<32> type @equals(0x...)` dispatch). Scanner-side legality is limited to the generic 8/16-byte header minima; the `uuid` 24/32-byte minima are **DSL semantics** (D3), not scanner semantics.

**`size`/`largesize` semantics**: per ISO/IEC 14496-12, `size` (and `largesize` for `size == 1`) is the **size of the entire box including its header**.

**Framing algorithm and pseudocode (separate `size == 0` branch, subtraction-based arithmetic)**:

At box start offset `start` within bounded region `[region_start, region_end)`:
Let `remaining = region_end - start`.

```text
if remaining < 8:
    // Truncated header: region ends, scanning stops cleanly.
    stop_scanning()

read u32 size, u32 type

if size == 0:
    // Terminal box extending to end of region (distinct branch, never runs start + box_size)
    span = [start, region_end)
    terminal = true
    truncated = false
    emit_record(span, terminal=true, truncated=false)
    stop_scanning()

if size == 1:
    if remaining < 16:
        // Incomplete large header (cannot read largesize)
        stop_scanning()
    read u64 largesize
    if largesize < 16:
        // Malformed large size: not a valid box, scanning stops immediately
        stop_scanning()
    box_size = largesize
else if size >= 8:
    box_size = size
else:
    // Malformed size in 2..7: not a valid box, scanning stops immediately
    stop_scanning()

// Size != 0 span calculation
if box_size <= remaining:
    span = [start, start + box_size)
    terminal = false
    truncated = false
    emit_record(span, terminal=false, truncated=false)
    advance(start + box_size)
else:
    // Box exceeds remaining bytes in region
    span = [start, region_end)
    terminal = true
    truncated = true
    emit_record(span, terminal=true, truncated=true)
    stop_scanning()
```

**Byte-to-bit coordinate protection and diagnostics**:
Before constructing bit addresses, offset and length values are validated:
`start <= std::numeric_limits<quint64>::max() / 8`, `span_length <= std::numeric_limits<quint64>::max() / 8`, and `span_length <= (std::numeric_limits<quint64>::max() / 8) - start`.
If `core::SourceSpan::create` fails due to coordinate bounds, the scanner produces an error with `core::DiagnosticCode::SourceError`, `core::DiagnosticSeverity::Error`, and the analysis node terminates in `core::MaterializationState::Invalid`.

**Detector candidate rating vs truncated records**:
- The candidate detector (`detectMp4Candidate`, P5d-1) counts **only complete, non-truncated, well-formed boxes**:
  - `Strong`: >= 3 complete boxes tiling the inspected prefix;
  - `Probable`: 2 complete boxes;
  - `Weak`: 1 complete box.
- Truncated box records (`truncated = true`) are emitted so the analyzer can materialize the partial box header with `core::DiagnosticCode::TruncatedSource` (`dsl_vm.cpp:3249-3252`), but they are **never counted** toward any candidate tier.
- A trailing truncated box does not downgrade previously accumulated complete boxes (e.g. 3 complete boxes + 1 trailing truncated box evaluates to `Strong`; a truncated first box yields no candidate `std::nullopt`).

**Scanner/DSL data contract — span-only, no header-value publication**:
The scanner outputs each box **span** (start offset, end offset, truncation flag). It does **not** publish `size`/`type`/`largesize` values. The runner maps the span to a source sub-view (`aac_adts_analyzer.cpp:309/:346` precedent), and the DSL `Box` struct **re-reads** `size`/`type`/`largesize` from the source within the span.

After P5d-1/P5d-2, `sequence<Box> boxes = scan(mp4_box);` (with `@index(progressive)`) becomes expressible.

---

### D2: Nested Drilling — `@container` Annotation, Runner Re-Entry, and Shared Budget

Container boxes (`moov`/`trak`/`mdia`/`minf`/`stbl`) need to enumerate child boxes across their payload byte region.

**Locked syntax** (post-position on a lazy `bytes` region, one argument, a struct identifier):

```svfmt
@lazy(payload_bytes) bytes payload @container(ChildStruct);
```

- **Host**: `DslAnnotationTarget::LazyRegion` only.
- **Argument contract**: exactly one argument; the token must parse as `DslAnnotationValueKind::Identifier` (`dsl.cpp:801`). The identifier must name a declared struct.
- **Diagnostics** (P5d-2):
  - unregistered name: `Unknown annotation '@container'` (`dsl.cpp:1880`, probe P2d/P8c);
  - host not LazyRegion: `@container is not supported on this declaration` (`dsl.cpp:1905-1907`);
  - arity != 1 or non-identifier argument: `InvalidAnnotation`;
  - target struct not declared: `UnknownReference` (`dsl.cpp:3556-3564` precedent).

**Parser AST**: `DslAnnotation { name = "container", arguments = [{ kind = Identifier, text = structName }] }` stored in the existing `annotations` vector of `DslLazyRegion` (`dsl.h:234-240`). No AST type changes.

**Typed IR**: `DslTypedField` (`dsl_ir.h:108-124`) gains `std::optional<quint32> containerChildStructIndex` (valid only when `type.kind == DslValueTypeKind::LazyBytes`). Resolved during `compileLazyRegion` (`dsl_ir.cpp:2462-2540`).

**Role of `RegisterLazyBytes` vs Session-Owned Runner**:
- `DslOpcode::RegisterLazyBytes` (`dsl_vm.cpp:3169-3320`) **only registers the lazy node** and stores metadata (`containerChildStructIndex`). It does **not** eagerly execute re-entry.
- Child box enumeration is driven by the session-owned `Mp4IsobmffAnalyzer` runner (P5d-3) re-entering the `Mp4Box` scanner over the container's byte span `[anchor_start, anchor_end)` and materializing child nodes using the named struct as the element type, attached under the container's lazy node.

**State transition matrix** (matching `AnalysisTree::canTransition`, `analysis_model.cpp:218-251`):

| From | To (allowed) | Meaning for a container node |
| :--- | :--- | :--- |
| Lazy | Indexing | Child enumeration begins (runner re-entry) |
| Lazy | WaitingDependency | Re-entry waits on context dependency |
| Lazy | Cancelled / Unsupported / Invalid / Materialized | Direct terminal transition (empty / error / pre-materialized) |
| Indexing | WaitingDependency | Mid-enumeration dependency wait |
| Indexing | Cancelled / Unsupported / Invalid / Materialized | Terminal states after child enumeration |
| WaitingDependency | Indexing | Dependency resolved; child enumeration resumes |
| WaitingDependency | Cancelled / Unsupported / Invalid / Materialized | Terminal while waiting |
| Cancelled | Indexing | resumeCancelled re-enters child enumeration |
| Unsupported / Invalid / Materialized | — | Terminal; no further transition |

**Runner-owned shared execution budget (`RunnerExecutionBudget`)**:
The session-owned runner maintains an active shared budget across all nested VM re-entries. Because analysis execution is strictly single-threaded within the session runner, `RunnerExecutionBudget` is accessed serially without mutex overhead:

```cpp
struct RunnerExecutionBudget {
    quint64 remainingNodes = 100'000;         // defaultMaximumMaterializedNodes
    quint64 remainingInstructions = 1'000'000;  // defaultMaximumInstructions
    quint32 currentNestingDepth = 0;          // bounded by defaultMaximumNodeDepth = 256
    std::shared_ptr<const std::atomic_bool> cancellation;
};
```

- Before calling VM `execute`, the per-invocation limits in `DslExecutionOptions` are narrowed to the shared balances (`limits.maximumMaterializedNodes = budget.remainingNodes`, `limits.maximumInstructions = budget.remainingInstructions`).
- If `budget.remainingNodes == 0` or `budget.remainingInstructions == 0`, the operation immediately returns `DslExecutionStatus::ResourceLimit`.
- After VM `execute` returns (regardless of success, truncation, or error), the runner performs checked subtraction: `budget.remainingNodes = (result.nodesCreated >= budget.remainingNodes) ? 0 : (budget.remainingNodes - result.nodesCreated)` and `budget.remainingInstructions = (result.instructionsExecuted >= budget.remainingInstructions) ? 0 : (budget.remainingInstructions - result.instructionsExecuted)`.
- `maximumViewDepth = 64` and `maximumCallDepth = 64` are per-invocation VM stack limits, not cumulative budgets.
- `currentNestingDepth` is incremented before child scan re-entry and decremented upon return; exceeding `maximumNodeDepth = 256` marks the container node as `ResourceLimit`.

**Core/runner neutrality**: Child struct is referenced by IR index only; **no FourCC literal enters core or runner code**.

---

### D3: Type Tag Matching — `bits<32>` + `@equals(0x...)`, UUID Wire Order and Underflow Semantics

Fact 10 holds: `bits<32> box_type @equals(0x66747970)` compiles; hex integer literals are admitted. Common FourCC constants:
`ftyp` (0x66747970), `moov` (0x6D6F6F76), `mdat` (0x6D646174), `free` (0x66726565), `mvhd` (0x6D766864), `trak` (0x7472616B), `tkhd` (0x746B6864), `edts` (0x65647473), `elst` (0x656C7374), `mdia` (0x6D646961), `mdhd` (0x6D646864), `hdlr` (0x68646C72), `minf` (0x6D696E66), `vmhd` (0x766D6864), `smhd` (0x736D6864), `dinf` (0x64696E66), `dref` (0x64726566), `stbl` (0x7374626C), `stsd` (0x73747364), `stts` (0x73747473), `stss` (0x73747373), `stsc` (0x73747363), `stsz` (0x7374737A), `stco` (0x7374636F), `avc1` (0x61766331), `avcC` (0x61766343), `mp4a` (0x6D703461), `esds` (0x65736473), `pasp` (0x70617370), `btrt` (0x62747274), `mvex` (0x6D766578), `trex` (0x74726578), `moof` (0x6D6F6F66), `mfhd` (0x6D666864), `traf` (0x74726166), `tfhd` (0x74666864), `trun` (0x7472756E), `udta` (0x75647461), `meta` (0x6D657461), `ilst` (0x696C7374).

**UUID Box Wire Order and Minimum Size Validation**:
Per ISO/IEC 14496-12:
- On wire for a large UUID (`size == 1`), `largesize` (bytes 8..15) precedes `usertype` (bytes 16..31).
- For normal UUID (`size >= 24`) and EOF UUID (`size == 0`), `usertype` immediately follows `type` (bytes 8..23).
- `usertype` must **not** be read unconditionally before branching on `size == 1`.
- Minimum payload size requirements: large UUID requires `largesize >= 32` (16-byte header + 16-byte usertype); normal UUID requires `size >= 24` (8-byte header + 16-byte usertype).
- If a malformed stream provides `16 <= largesize < 32` or `8 <= size < 24`, the subtraction in `largesize - 32` or `size - 24` triggers the VM's checked unsigned arithmetic underflow, producing `DslExecutionStatus::InvalidSyntax` with `"Unsigned subtraction underflow in computed field"` anchored at the computed field.

```svfmt
struct UuidBox {
    bits<32> size;
    bits<32> type @equals(0x75756964);
    if (size == 1) {
        bits<64> largesize;
        bits<64> large_usertype_hi;
        bits<64> large_usertype_lo;
        computed<u64> large_payload = largesize - 32;
        @lazy(large_payload) bytes large_data;
    } else {
        bits<64> usertype_hi;
        bits<64> usertype_lo;
        if (size == 0) {
            computed<u64> payload_bytes = available_bytes();
            @lazy(payload_bytes) bytes payload;
        } else {
            computed<u64> payload = size - 24;
            @lazy(payload) bytes data;
        }
    }
}
```

Probe P8d confirms the syntax and IR legality of this correct wire order and branching structure (`Rule OK`). Probe P8e confirms that `available_bytes()` is currently the first missing builtin rejected by the compiler (`error: Pure function is not declared before this call`, `dsl_ir.cpp:772`).

---

### D4: Size Branches — Normal Box 3-Way Branching and `available_bytes()` Builtin

**Normal Box 3-Way Branching Structure**:
To prevent `size == 0` from executing `size - 8` (which causes runtime subtraction underflow on unsigned integers), normal boxes use the following nested branching structure:

```svfmt
struct Box {
    bits<32> size;
    bits<32> type;
    if (size == 1) {
        bits<64> largesize;
        computed<u64> large_payload = largesize - 16;
        @lazy(large_payload) bytes large_data;
    } else {
        if (size == 0) {
            computed<u64> payload_bytes = available_bytes();
            @lazy(payload_bytes) bytes payload;
        } else {
            computed<u64> payload = size - 8;
            @lazy(payload) bytes data;
        }
    }
}
```

**`available_bytes()` Builtin Contract (P5d-2)**:
- Returns remaining bytes in current source reader from current bit position: `reader.remainingBits() / 8`.
- Registered in `compileExpression` (`dsl_ir.cpp:589-611`, `DslTypedExpressionKind::AvailableBytes`) and evaluated in `dsl_vm.cpp:496-499`.
- Non-negative `u64` scalar value usable in any `computed` expression.

---

### D5: `@target_format` — Registration, Metadata, Cache, and `resolveByFormat`

**Locked syntax** (post-position on a lazy `bytes` region, one string argument):

```svfmt
@lazy(payload_bytes) bytes payload @target_format("video/mp4");
```

- **Host**: `DslAnnotationTarget::LazyRegion` only.
- **Registry change** (P5d-2): `knownAnnotations` at `dsl.cpp:1834-1866` updates `{u"target_format", static_cast<quint32>(DslAnnotationTarget::LazyRegion)}`; lazy region whitelist message (`dsl.cpp:1895`) extended to admit `@target_format`.
- **Typed IR**: `DslTypedField` gains `std::optional<QString> targetFormat`.
- **Node metadata**: `core::AnalysisNodeMetadata` (`analysis_model.h:76-80`) gains `std::optional<QString> targetFormat`.
- **Cache encoding**: `nodeTargetFormatFlag = 4U` in `analysis_cache_payload.cpp:35-36`. Old caches without the flag bit decode with `targetFormat = std::nullopt`.
- **`RulePackageCatalog::resolveByFormat` service (P5d-2)**:
  `resolveByFormat(QStringView format, QStringView runningLanguage, QStringView runningEngine)` scans packages for an entry point matching `RulePackageEntryPoint::format` (`rule_package.h:61-69`), returning `Found`, `MissingContent`, or `VersionConflict`.
- **P5d vs P5i**: P5d-2 delivers the annotation, metadata, cache flag, and `resolveByFormat`; P5i consumes it in the UI navigation action.

---

### D6: Sample Table Windowing — Dedicated `@window` Annotation and Decoder Contract

`@container` is strictly reserved for `Mp4Box` scanner re-entry over box streams. For fixed-width sample table windowing (`stts`, `stsc`, `stsz`, `stco`, `co64`), a dedicated `@window` annotation is defined.

**Locked syntax** (post-position on a lazy `bytes` region, two identifier arguments):

```svfmt
@lazy(table_bytes) bytes entries @window(EntryStruct, entry_count_field);
```

- **Host**: `DslAnnotationTarget::LazyRegion` only.
- **Arguments**: exactly two arguments, both `DslAnnotationValueKind::Identifier` (the entry struct type name, and the count field name declared in the enclosing struct).
- **Diagnostics** (P5d-2):
  - unregistered name (today): `Unknown annotation '@window'` (`dsl.cpp:1880`, probes P9a/P9b verify rejection at the name gate);
  - host not LazyRegion: `@window is not supported on this declaration`;
  - arity != 2 or non-identifier argument: `InvalidAnnotation`;
  - undeclared entry struct: `UnknownReference`;
  - undeclared count field / non-scalar: `UnknownReference` / `InvalidType`.

**Fixed-width `EntryStruct` constraints (v0.1)**:
- Must contain ONLY unconditional, source-backed, static `bits<N[, endian]>` scalar fields or fixed-length arrays of static `bits<N>`.
- Prohibited in `EntryStruct`: dynamic-width fields, `ue`, `se`, `ff_coded`, conditional branches (`if`/`else`), loops (`repeat`/`until`), `computed` fields, `@lazy` regions, `compressed_payload`, `unsupported` statements, nested structs, or annotations other than `@description`/`@spec`.
- `entrySizeBits` is computed at compile time as the sum of all field bit widths in `EntryStruct`:
  - Must be strictly `> 0`;
  - Must be byte-aligned (`entrySizeBits % 8 == 0`);
  - Overflow check on `quint64` sum.

**Count field constraints**:
- `entry_count_field` must be declared earlier in the enclosing struct, before the lazy region.
- Must be an unconditional, scalar unsigned integer field.
- `RegisterLazyBytes` snapshots `entryCount` into the node's window metadata.

**Typed IR**: `DslTypedField` gains:
- `std::optional<quint32> windowEntryStructIndex`;
- `std::optional<quint32> windowEntryCountFieldIndex`;
- `std::optional<quint64> windowEntrySizeBits`.

**Node metadata / Session query**: `AnalysisNodeMetadata` / session interface exposes window metadata (`windowEntryStructIndex`, `windowEntrySizeBits`, `entryCount`). UI and session callers read this metadata directly without guessing struct index or entry count.

**`WindowDecoder` Session-Owned Object (`src/rules/include/streamview/rules/window_decoder.h`, P5d-3)**:

```cpp
class WindowDecoder final {
public:
    explicit WindowDecoder(
        const DslTypedProgram& program,
        const core::RandomAccessSource& source,
        core::SourceMapping sourceMapping,
        std::shared_ptr<core::AnalysisTree> tree,
        core::AnalysisNodeId containerNodeId,
        std::shared_ptr<RunnerExecutionBudget> budget,
        std::optional<core::CancellationToken> cancellation = std::nullopt);

    [[nodiscard]] WindowDecodeResult decodeWindow(const WindowDecodeRequest& request);
};
```

**`DslExecutionStatus` Full 9-State Matrix and Window Return Subset**:
The complete 9 states defined in `dsl_vm.h:18-28`:
1. `Materialized`
2. `Unsupported`
3. `TruncatedSource`
4. `InvalidSyntax`
5. `DependencyUnavailable`
6. `SourceError`
7. `Cancelled`
8. `ResourceLimit`
9. `InvalidDefinition`

Window decoding returns a subset of these states:
- `Materialized`: requested page successfully decoded;
- `TruncatedSource`: lazy region or source shorter than requested page entries;
- `ResourceLimit`: shared node count or instruction budget exhausted;
- `Cancelled`: cancellation token triggered during decoding;
- `SourceError`: I/O or bit-coordinate error reading source bytes;
- `InvalidDefinition`: invalid `entryStructIndex` or malformed request parameters.

**Boundary, checked arithmetic, and cache contracts**:
- `entryCount` is clamped to available region capacity: `clampedCount = min(entryCount, availableRegionBytes / (entrySizeBits / 8))`;
- Checked multiplication `pageIndex * pageSize` with overflow check;
- Checked addition `pageStartIndex + pageCount` with overflow check;
- Checked multiplication `entryIndex * entrySizeBits` and byte offset/length mapping;
- Lazy region boundary check: if requested page exceeds available range, decodes only available entries and returns `TruncatedSource`;
- **Window metadata cache serialization (P5d-2)**:
  - Cache flag `nodeWindowMetadataFlag = 8U` in `analysis_cache_payload.cpp`;
  - Encodes `windowEntryStructIndex` (`quint32`), `windowEntryCountFieldIndex` (`quint32`), `windowEntrySizeBits` (`quint64`), `entryCount` (`quint64`);
  - Backwards compatibility: old cache payloads without flag bit `8U` decode with window metadata `std::nullopt`.

---

### D7: Fragmented MP4 (`moof`) — In-Rule `unsupported` Contract

- **Compile evidence (P5c, verified)**: `unsupported("fragmented MP4 (moof/traf) is outside the v0.1 subset") at type;` compiles — probe P7d, `Rule OK`.
- **P5d-3 Runner Normative Behaviors (asserted in tests)**:
  1. `moof` box node is materialized with `MaterializationState::Unsupported`, `DiagnosticCode::UnsupportedSyntax`, `DiagnosticSeverity::Warning`, anchored at `type`;
  2. `moof` prefix fields `size` and `type` remain materialized;
  3. Prior top-level boxes (`ftyp`, `moov`, ...) remain materialized;
  4. Top-level scanning continues: boxes following `moof` in the region (e.g. `mdat`) are materialized.
- Test fixture is committed/generated via script under `tests/fixtures/` (P5d-3), never referencing `/tmp`.

---

## Rejected Alternatives

1. **Struct-typed fields** (`BoxHeader header;`) for nesting — rejected: parser rejects struct types as field types (probe P2a); `@container` achieves nesting without new types.
2. **Nested `sequence` declarations** — rejected: requires new grammar and VM semantics; conflicts with top-level-only sequence contract (probe P2b).
3. **New `payload<box>` view kind** — rejected for v0.1: dispatch view selects one struct while containers enumerate many children (probe P2c).
4. **`bits<128>` for UUID** — rejected: not expressible (`Bit field width must be in the range 1..64`, `dsl.cpp:1038`, probe P8b); locked form is two `bits<64>` fields (probe P8a).
5. **Whole-UUID literal equality** — rejected for v0.1: no string-literal tokenizer or bytes value type.
6. **Detector-grade fragmented-MP4 rejection** — rejected (D7): loses decoded prefix and prevents subsequent-box scanning.
7. **Scanner header-value publication** — rejected: span-only contract (D1) matches existing runner record mapping (`aac_adts_analyzer.cpp:309/:346`).
8. **Scanner-computed remaining-length pass-down for `size == 0`** — rejected in favor of `available_bytes()` builtin (D4).
9. **Reusing `@container` for sample table windowing** — rejected (D6): `@container` triggers `Mp4Box` scanner re-entry; sample tables require dedicated `@window(Entry, count)` binding for fixed-width records.
10. **`@target_format` carrying full package identity** — rejected (D5): couples rule text to packaging details; `resolveByFormat` service keeps annotation a clean format string.

---

## Consequences

### Positive
- ISOBMFF box tree is fully expressible after P5d: top-level framing (D1), container re-entry with shared execution budget (D2), correct UUID wire order with deterministic underflow diagnostics (D3), safe 3-way branching (D4), cross-format navigation metadata (D5), dedicated `@window` sample table binding (D6), and normative fragmented MP4 handling (D7).
- Core engine remains 100% format-neutral (no FourCC in core).
- All capabilities map to real C++ types and have directly testable contracts.

### Negative
- P5d delivers four capability increments across three sub-slices:
  - **P5d-1**: `Mp4Box` scanner + detector (`detectMp4Candidate`).
  - **P5d-2**: Language/compiler/IR increments (`@container`, `@window`, `available_bytes()`, `@target_format`, metadata, cache, `RulePackageCatalog::resolveByFormat`).
  - **P5d-3**: `Mp4IsobmffAnalyzer` runner skeleton + container re-entry + shared execution budget + window decoder (`window_decoder.h`) + D7 continuation/fixture tests.

---

## Verification Matrix and Evidence

All 18 probes were executed with `build/dev/tools/svtool/svtool` (`svtool 0.1.0 (DSL 0.1)`); scratch sources live outside the repository.

| # | Probe | Command | Normalized result |
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
| P8d | uuid full size branches (correct wire order) | `svtool rule check scratch/p5c_p8d_uuid_full_branches.svfmt` | `Rule OK` |
| P8e | uuid `size == 0` gap (`available_bytes()`) | `svtool rule check scratch/p5c_p8e_uuid_size0_gap.svfmt` | `error: Pure function is not declared before this call` (`dsl_ir.cpp:772`) |
| P9a | `@window(Entry, entry_count)` post-position | `svtool rule check scratch/p5c_p9a_window_annotation.svfmt` | `error: Unknown annotation '@window'` (`dsl.cpp:1880`) |
| P9b | `@window(123, 456)` non-identifier argument | `svtool rule check scratch/p5c_p9b_window_token_kind.svfmt` | `error: Unknown annotation '@window'` (`dsl.cpp:1880`) |

**Fixture generation commands and verified sizes:**

- Regular fixture:
  `ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=duration=0.2:size=64x48:rate=10 -f lavfi -i sine=frequency=440:duration=0.2 -c:v libx264 -preset ultrafast -c:a aac -y /tmp/p5c_fixture.mp4`
  => exit code 0, 6,513 bytes (`-rw-r--r--@ 1 yun wheel 6513`).
- Fragmented fixture:
  `ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=duration=0.3:size=64x48:rate=10 -c:v libx264 -preset ultrafast -movflags frag_keyframe+empty_moov -y /tmp/p5c_frag.mp4`
  => exit code 0, 4,505 bytes (`-rw-r--r--@ 1 yun wheel 4505`).

**Box-level ground truth (`ffprobe -v trace`, relevant excerpt via `grep`)** — regular fixture (6,513 bytes):

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

**Fragmented fixture (`ffprobe -v trace`, relevant excerpt via `grep`)** — 4,505 bytes:

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
0.000000,0.100000,306,K__      (frame 1: pts 0.0s, duration 0.1s, absolute byte pos 306, keyframe)
0.100000,0.100000,3282,___     (frame 2: pts 0.1s, pos 3282, non-keyframe)
```

---

## References

- ADR-0096: MP4/ISOBMFF Container Architecture, Box Traversal, Cross-Layer Navigation, and Sample Indexing Boundaries.
- ADR-0098: Unrecognized Annotation Compiler Gate and Explicit Unsupported Syntax.
- ADR-0040: Non-Fatal Syntax Warnings and Range Annotations.
- ISO/IEC 14496-12:2015 (ISOBMFF box structure and `size`/`largesize`/`size == 0` semantics; `uuid` 16-byte `usertype`).
- Task P5c/P5c-R/P5c-R2/P5c-R3/P5c-R4 definition and plan facts 10-16, `Sub-Agent分步开发指导计划.md`.
