# ADR-0095: AAC Raw Data Block Compressed Payload and Profile Handling

- **Status**: Proposed
- **Date**: 2026-08-16
- **Authors**: StreamView Contributors
- **Area**: DSL Rules / AAC Decoding / Analyzer Execution

## Context

Phase 4 of the StreamView implementation plan specifies full structural support for AAC-LC audio (ISO/IEC 14496-3:2019, Edition 5). Following the completion of ADTS header structured decoding (ADR-0093, Task T16) and AudioSpecificConfig / Program Config Element decoding (ADR-0094, Task T17c), Phase 4 requires completing three final milestones (`docs/implementation-plan.md:196-198`):
1. Marking `raw_data_block` as a compressed payload region without performing Huffman decoding;
2. Formally handling and reporting HE-AAC, ELD, and other AAC profiles;
3. Conducting a bit-by-bit acceptance audit across ADTS headers, ASC/PCE, truncated streams, CRC error/presence, and profile constraints.

### Empirical Probing Findings

To ensure precise architectural design, the following technical facts were established via empirical probes:

1. **DSL Syntax Capability (No Language Extension Needed)**:
   Adding a lazy byte region to `AdtsHeader` in `src/rules/official/org.streamview.aac/src/aac_adts.svfmt`:
   ```dsl
   computed<u64> header_bytes = (protection_absent == 1) * 7 + (protection_absent == 0) * 9;
   computed<u64> raw_data_block_bytes = aac_frame_length - header_bytes;
   @lazy(raw_data_block_bytes) bytes raw_data_block
       @spec("ISO/IEC 14496-3:2019", "1.A.1")
       @description("Raw audio data payload block.");
   ```
   Passes `svtool rule check` cleanly (`Rule OK`). The conditional `crc_check` (16 bits) preserves strict byte alignment, and because the `@lazy` region resides at the end of `AdtsHeader`, it does not trigger conservative alignment rejections (`docs/format-language/README.md:578-582`).

2. **Analyzer View Mapping Blockage**:
   `src/rules/aac_adts_analyzer.cpp:346` currently constructs a `SourceMapping` covering only `*record.headerSpan` (7 or 9 bytes). When `AdtsHeader` contains `@lazy(raw_data_block_bytes)`, evaluating a 20-byte ADTS frame against a 7-byte header view fails at `src/rules/dsl_vm.cpp:3143` with `DslExecutionStatus::TruncatedSource` and diagnostic `Lazy byte region exceeds the available source range`.
   When the source mapping is expanded to the full frame span (`*record.frameSpan`, 20 bytes), the DSL VM successfully materializes the frame with `raw_data_block` in `MaterializationState::Lazy` (bit offset 56, length 104 bits).

3. **Truncation Semantics Shift**:
   When an ADTS frame is truncated at EOF (e.g. declaring 20 bytes but only 15 bytes available in the file), the DSL VM encounters `bitCount > reader.remainingBits()` during lazy region registration (`src/rules/dsl_vm.cpp:3140-3149`), emitting `DslExecutionStatus::TruncatedSource` with Error severity.
   This replaces the existing ad-hoc C++ warning (`ADTS frame payload is truncated at EOF`, Severity: Warning, `src/rules/aac_adts_analyzer.cpp:456-467`) and requires updating test assertions in `tests/rules/aac_adts_analyzer_test.cpp:321-361`.

4. **Underflow Protection**:
   If a malformed frame declares `aac_frame_length < 7` (or `< 9` with CRC), the existing assertion `assert(aac_frame_length >= minimum_frame_length)` is evaluated before the lazy payload, terminating execution with `InvalidSyntax` (`Assertion condition is false`) and preventing negative/underflowing lazy byte allocations.

5. **Unsupported Profile Reporting Mechanisms in DSL**:
   Auditing `src/rules/` confirms that no mechanism currently exists in the DSL to emit `MaterializationState::Unsupported` or `DiagnosticCode::UnsupportedSyntax`:
   - `@enum` violation is fatal (`DiagnosticCode::InvalidSyntax`, Severity: Error), rejecting frame decoding entirely.
   - `@range(min, max)` only accepts a single continuous interval and cannot express non-continuous profile IDs (e.g. AOT 2, 5, 29, 39).
   - In ADTS, the 2-bit `profile` field (Table 1.A.1) only encodes 0..3 (Main, LC, SSR, LTP) and cannot encode AOT 5 (SBR), 29 (PS), or 39 (ELD). In practical broadcast streams, HE-AAC streams in ADTS declare `profile = 1` (LC) in the ADTS header and convey SBR/PS extensions inside the `raw_data_block`.

---

## Decision

### 1. Specification Citations for `raw_data_block`

In accordance with ISO/IEC 14496-3:2019 (Edition 5):
- **ADTS Payload Sequencing**: Subpart 1 Annex 1.A subclause **1.A.1** (*Fixed and variable header of ADTS*) and Table 1.A.5 (`adts_frame()`).
- **Raw Data Block Syntax Definition**: Subpart 4 (General Audio) subclause **4.5.2.1** (*raw_data_block* / *Syntactic elements*).

In `aac_adts.svfmt`, `@lazy(raw_data_block_bytes) bytes raw_data_block` is attributed to Subclause `1.A.1`.

### 2. DSL Rule Definition for `raw_data_block`

In `src/rules/official/org.streamview.aac/src/aac_adts.svfmt`, append the following structure fields to `AdtsHeader`:

```dsl
    computed<u64> header_bytes = (protection_absent == 1) * 7 + (protection_absent == 0) * 9;
    computed<u64> raw_data_block_bytes = aac_frame_length - header_bytes;
    @lazy(raw_data_block_bytes) bytes raw_data_block
        @spec("ISO/IEC 14496-3:2019", "1.A.1")
        @description("Raw audio data payload block.");
```

### 3. Analyzer Frame View Mapping (Capability Slice T18b)

In `src/rules/aac_adts_analyzer.cpp:346`, update the logical view construction from `{*record.headerSpan}` to `{*record.frameSpan}`.
The scanner (`src/rules/aac_adts_scanner.cpp:134-148`) already bounds `record.frameSpan` to `availableBytes`. Mapping the full frame span allows the DSL VM to decode the header fields and immediately register the lazy payload region across the actual frame payload bytes.

### 4. Harmonized Truncation Contract (Option A)

We adopt **Option A** (universal DSL VM truncation semantics):
- When a stream ends prematurely in the middle of a frame's payload, the DSL VM attempts to allocate `raw_data_block_bytes` and detects that `bitCount > reader.remainingBits()`.
- The DSL VM emits `DslExecutionStatus::TruncatedSource` with Error severity and message `Lazy byte region exceeds the available source range`.
- The frame node is marked as `Invalid`, retaining its decoded header field children and source-anchored location.
- The ad-hoc C++ warning at `aac_adts_analyzer.cpp:456-467` is superseded by this native VM diagnostic.
- `tests/rules/aac_adts_analyzer_test.cpp:321-361` (`handlesPayloadTruncationAtEof`) is migrated to assert `DiagnosticSeverity::Error`, message `Lazy byte region exceeds the available source range`, and `header1->state() == MaterializationState::Invalid`.

### 5. Profile Handling and Explicit Capability Boundaries

1. **ADTS Transport Streams**: Constrained to `AacProfile` (0=Main, 1=LC, 2=SSR, 3=LTP) via `@enum(AacProfile)`. HE-AAC v1/v2 streams transmitted via ADTS carry `profile = 1` (LC) in the ADTS header per broadcast standards, decoding successfully as LC frames with unparsed raw data payloads.
2. **AudioSpecificConfig (ASC)**: `audio_object_type` values outside General Audio (e.g. SBR=5, PS=29, ELD=39) decode the GA baseline header syntax without syntax failure (`MaterializationState::Materialized`), as established in ADR-0094 §3:315. This is formally accepted as a documented capability boundary until dedicated non-GA payload decoders are introduced in future milestones.

### 6. Phase 4 Task Slicing and Discipline

To uphold strict single-responsibility commits and keep capability changes separate from rule consumption:

- **Task T18a** (Current): Probing conclusions, bilingual ADR-0095, implementation plan update (Markdown-only).
- **Task T18b**: Analyzer view mapping update to frame span (`src/rules/aac_adts_analyzer.cpp:346`), runner capability slice without package version bump.
- **Task T18c**: Rule consumption of `@lazy raw_data_block` in `aac_adts.svfmt`, package version bump to `0.1.3`, test suite updates.
- **Task T18d**: Profile handling verification & documentation alignment.
- **Task T18e**: Bit-by-bit audit across 5 categories, Phase 4 checkbox completion (`docs/implementation-plan.md:196-198`), Phase advancement to Phase 5.

---

## Verification Matrix

| Probe / Verification Item | Target File / Command | Observed Output & Confirmation |
| :--- | :--- | :--- |
| **P1: DSL Syntax Check** | `svtool rule check scratch/probe_adts_lazy.svfmt` | `Rule OK` (rc=0) |
| **P2: 20-byte Frame Full Mapping** | `scratch/probe_lazy_raw_data_block` (Test A) | `status=0 materialized=1 AdtsHeader state=6 children=19 child=raw_data_block state=0 bitOffset=56 bitLength=104` |
| **P3: 15-byte Truncated Frame** | `scratch/probe_lazy_raw_data_block` (Test B) | `status=1 materialized=0 AdtsHeader state=5 diag code=0 sev=2 msg="Lazy byte region exceeds the available source range"` |
| **P4: 7-byte Header-only Mapping** | `scratch/probe_lazy_raw_data_block` (Test C) | `status=1 materialized=0 AdtsHeader state=5 diag code=0 sev=2 msg="Lazy byte region exceeds the available source range"` |
| **P5: @range Non-contiguous Check** | `scratch/probe_unsupported` (Probe 3) | `Parse succeeded=0 diag code=14 msg="@range requires two integer arguments"` |

---

## References

- ISO/IEC 14496-3:2019, Information technology — Coding of audio-visual objects — Part 3: Audio (Subpart 1 Annex 1.A, Subpart 4 subclauses 4.4.1, 4.4.1.1, 4.5.2.1).
- [ADR-0040: Non-Fatal Syntax Warnings and Range Annotations](0040-non-fatal-syntax-warnings-and-range-annotations.md)
- [ADR-0092: AAC ADTS Frame Enumeration and Rule Package Architecture](0092-aac-adts-frame-enumeration-and-rule-package.md)
- [ADR-0093: ADTS Header Structured Decoding and Official Rule Package](0093-adts-header-structured-decoding-and-official-rule-package.md)
- [ADR-0094: AudioSpecificConfig and Program Config Element Structured Decoding](0094-audio-specific-config-and-program-config-element.md)
