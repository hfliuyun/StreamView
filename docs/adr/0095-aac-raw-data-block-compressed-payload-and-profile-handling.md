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

To establish deterministic facts prior to code modifications, the following technical observations were verified empirically:

1. **DSL Syntax Capability and Minimum Field Footprint (N1)**:
   In `src/rules/official/org.streamview.aac/src/aac_adts.svfmt`, `AdtsHeader` already declares:
   ```dsl
   computed<u64> minimum_frame_length = (protection_absent == 1) * 7 + (protection_absent == 0) * 9;
   assert(aac_frame_length >= minimum_frame_length) at aac_frame_length;
   ```
   Appending the lazy byte region by directly reusing `minimum_frame_length`:
   ```dsl
   computed<u64> raw_data_block_bytes = aac_frame_length - minimum_frame_length;
   @lazy(raw_data_block_bytes) bytes raw_data_block
       @spec("ISO/IEC 14496-3:2019", "4.5.2.1")
       @description("Raw audio data payload block.");
   ```
   Passes `svtool rule check` cleanly (`Rule OK`, rc=0). Reusing `minimum_frame_length` avoids introducing a redundant `header_bytes` computed node and prevents duplication of the 7/9 header size expression. Because the conditional `crc_check` (16 bits) preserves strict byte alignment and `@lazy` resides at the end of the structure, it conforms to the byte boundary rules in `docs/format-language/README.md:578-582`.

2. **Analyzer View Mapping Blockage and T18b Independence**:
   `src/rules/aac_adts_analyzer.cpp:346` currently constructs a `SourceMapping` covering only `*record.headerSpan` (7 or 9 bytes). When `AdtsHeader` contains `@lazy(raw_data_block_bytes)`, evaluating a 20-byte ADTS frame against a 7-byte header view fails at `src/rules/dsl_vm.cpp:3143` with `DslExecutionStatus::TruncatedSource` and diagnostic `Lazy byte region exceeds the available source range`.
   When the source mapping is expanded to the full frame span (`*record.frameSpan`, 20 bytes), the DSL VM successfully materializes the frame with `raw_data_block` in `MaterializationState::Lazy` (bit offset 56, length 104 bits; or bit offset 72, length 88 bits with CRC).
   Crucially, expanding the runner source mapping to `*record.frameSpan` against the current official rule package (`org.streamview.aac` 0.1.2 without the lazy payload) is an exact no-op: 7-byte header views, 20-byte frame views, and 15-byte truncated views all produce `status=0 materialized=1` identically.

3. **Truncation Semantics Shift and Harmonization**:
   When an ADTS frame is truncated at EOF (e.g. declaring 20 bytes but only 15 bytes available in the file), the DSL VM encounters `bitCount > reader.remainingBits()` during lazy region registration (`src/rules/dsl_vm.cpp:3140-3149`), emitting `DslExecutionStatus::TruncatedSource` with Error severity.
   This replaces the existing ad-hoc C++ warning (`ADTS frame payload is truncated at EOF`, Severity: Warning, `src/rules/aac_adts_analyzer.cpp:456-467`) and harmonizes ADTS truncation with the universal DSL VM truncation contract across all formats (ADR-0092 §1.3).

4. **Underflow Protection**:
   If a malformed frame declares `aac_frame_length < 7` (or `< 9` with CRC), the existing assertion `assert(aac_frame_length >= minimum_frame_length)` is evaluated before the lazy payload, terminating execution with `InvalidSyntax` (`Assertion condition is false`) and preventing negative/underflowing lazy byte allocations.

5. **Profile Reporting Constraints in Current DSL**:
   Auditing `src/rules/` confirms that no mechanism currently exists in the DSL to emit `MaterializationState::Unsupported` or `DiagnosticCode::UnsupportedSyntax`:
   - `@enum` violation is fatal (`DiagnosticCode::InvalidSyntax`, Severity: Error, `src/rules/dsl_vm.cpp:2782-2799`), which would reject frame decoding rather than non-fatally reporting unsupported capability.
   - Non-contiguous value sets (e.g. AOT 2, 5, 29, 39) cannot be expressed via `@range`: `@range` requires exactly two integer arguments (`src/rules/dsl.cpp:2560` / `src/rules/dsl_ir.cpp:183`) and duplicate `@range` annotations on a single field are rejected by compiler gate (`src/rules/dsl.cpp:2534` / `src/rules/dsl_ir.cpp:173`, `@range may appear at most once on a field`).
   - In ADTS headers, the 2-bit `profile` field (ISO/IEC 14496-3 Table 1.A.1) only encodes 0..3 (Main, LC, SSR, LTP); it cannot represent AOT 5 (SBR), 29 (PS), or 39 (ELD). In standard broadcast streams, HE-AAC streams in ADTS declare `profile = 1` (LC) in the ADTS header and convey SBR/PS extensions in-band inside `raw_data_block`.

6. **Silent Ignorance of Unrecognized Annotations (N2)**:
   Probing revealed that `DslCompiler` (`src/rules/dsl_ir.cpp`) validates known annotations (`@equals`, `@range`, `@enum`, `@spec`, `@description`, `@context_export`) but silently ignores unrecognized annotations on declared bit fields. For instance, `bits<12> syncword @equalss(4095);` compiles with `Rule OK`, and executing against an invalid stream (`syncword=255`) yields `status=Materialized`, `materialized=1`, `diags=0`, whereas `@equals(4095)` correctly emits `InvalidSyntax` / `Field value violates @equals constraint`. This behavior is recorded as an explicit observation and escalated for future language gate hardening.

---

## Decision

### 1. Specification Citations for `raw_data_block` (B2, N4)

In ISO/IEC 14496-3:2019 (Edition 5):
- **ADTS Payload Sequencing**: Subpart 1 Annex 1.A subclause **1.A.1** (*Fixed and variable header of ADTS*) and Table 1.A.5 (`adts_frame()`), where `adts_frame()` sequences `adts_fixed_header()`, `adts_variable_header()`, optional `adts_error_check()`, followed by `raw_data_block()`.
- **Raw Data Block Syntax Definition**: Subpart 4 (General Audio) subclause **4.5.2.1** (*raw_data_block* / *Syntactic elements*), which standardizes the grammatical element composition of `raw_data_block()` (SCE, CPE, LFE, DSE, PCE, FIL, TERM).

To maintain strict normative precision, `@spec("ISO/IEC 14496-3:2019", "4.5.2.1")` is assigned to `raw_data_block`, distinguishing the payload's syntax definition from the `1.A.1` fixed/variable header clauses.

### 2. DSL Rule Definition for `raw_data_block` (N1)

In `src/rules/official/org.streamview.aac/src/aac_adts.svfmt`, append the following structure fields to `AdtsHeader`:

```dsl
    computed<u64> raw_data_block_bytes = aac_frame_length - minimum_frame_length;
    @lazy(raw_data_block_bytes) bytes raw_data_block
        @spec("ISO/IEC 14496-3:2019", "4.5.2.1")
        @description("Raw audio data payload block.");
```

### 3. Analyzer Frame View Mapping (Capability Slice T18b)

In `src/rules/aac_adts_analyzer.cpp:346`, update the logical view construction from `{*record.headerSpan}` to `{*record.frameSpan}`.
Because the scanner (`src/rules/aac_adts_scanner.cpp:134-148`) bounds `record.frameSpan` to `availableBytes`, mapping the full frame span allows the DSL VM to decode the header fields and immediately register the lazy payload region across the frame payload bytes.

### 4. Harmonized Truncation Contract and Test Migration (N5)

We adopt the universal DSL VM truncation contract (Option A):
- When a stream ends prematurely in the middle of a frame's payload, the DSL VM attempts to allocate `raw_data_block_bytes` and detects that `bitCount > reader.remainingBits()`.
- The DSL VM emits `DslExecutionStatus::TruncatedSource` with Error severity and message `Lazy byte region exceeds the available source range`.
- The frame node is marked as `Invalid`, retaining its decoded header field children and source-anchored location.
- The legacy C++ synthetic warning at `aac_adts_analyzer.cpp:456-467` is superseded by this native VM diagnostic.

**Test Suite Migrations required in Task T18c**:
- `tests/rules/aac_adts_analyzer_test.cpp:143-170` (frame 0 without CRC): child node count advances from 16 to 18; ordered names list appends `raw_data_block_bytes` and `raw_data_block`.
- `tests/rules/aac_adts_analyzer_test.cpp:193-220` (frame 1 with CRC): child node count advances from 17 to 19; ordered names list appends `raw_data_block_bytes` and `raw_data_block`.
- `tests/rules/aac_adts_analyzer_test.cpp:114` (`handlesSingleAdtsFrame`): child node count advances from 16 to 18.
- `tests/rules/aac_adts_analyzer_test.cpp:210` (`decodesAdtsFrameWithCrc`): child node count advances from 17 to 19.
- `tests/rules/aac_adts_analyzer_test.cpp:321-361` (`handlesPayloadTruncationAtEof`):
  - Line 352: `DiagnosticSeverity::Warning` $	o$ `DiagnosticSeverity::Error`;
  - Line 353: `"ADTS frame payload is truncated at EOF"` $	o$ `"Lazy byte region exceeds the available source range"`;
  - Line 360: `header1->state() == MaterializationState::Materialized` $	o$ `header1->state() == MaterializationState::Invalid`;
  - Line 356: `node1->children().size() == 1` remains unchanged.

### 5. Profile Handling and Explicit Capability Boundaries

1. **ADTS Transport Streams**: Constrained to `AacProfile` (0=Main, 1=LC, 2=SSR, 3=LTP) via `@enum(AacProfile)`. HE-AAC v1/v2 streams transmitted via ADTS carry `profile = 1` (LC) in the ADTS header per broadcast standards, decoding successfully as LC frames with unparsed raw data payloads.
2. **AudioSpecificConfig (ASC)**: `audio_object_type` values outside General Audio (e.g. SBR=5, PS=29, ELD=39) decode the GA baseline header syntax without syntax failure (`MaterializationState::Materialized`), as established in ADR-0094 §3:315. This is formally accepted as a documented capability boundary until dedicated non-GA payload decoders are introduced in future milestones.

### 6. Bit-by-Bit Acceptance Audit Scope and Coverage Matrix (B1)

Phase 4 Item 5 (`docs/implementation-plan.md:198`) requires bit-by-bit verification across five distinct categories.

#### Definition of "CRC Error / Presence" Acceptance Boundary
StreamView rules do NOT perform CRC-16 polynomial division or arithmetic checksum calculation (just as Huffman decoding is not performed in DSL). The acceptance criteria for CRC error / presence are bounded to:
1. `protection_absent == 0`: The 16-bit `crc_check` field is materialized at exact bit offset 56..71 (bytes 7..8).
2. `protection_absent == 1`: `crc_check` is omitted; `raw_data_block` payload starts at exact bit offset 56 (byte 7).
3. `protection_absent == 0` with frame length < 9 bytes: Emits `TruncatedSource` diagnostic without crashing.

#### Coverage and Gap Inventory

| Category | Existing Verified Coverage (`file:line`) | Identified Gaps to be Closed in Task T18e |
| :--- | :--- | :--- |
| **1. ADTS Headers** | `tests/rules/aac_adts_analyzer_test.cpp:165-170` (ordered names), `:172-183` (values of indices 0-7, 12-15). | 1. Indices 8–11 (`original_copy`, `home`, `copyright_identification_bit`, `copyright_identification_start`) currently lack value assertions in all tests.<br>2. `logicalRange()`, `bitOffset()`, and `bitLength()` assertions are currently absent across all ADTS tests (0 assertions). |
| **2. ASC / PCE** | `tests/rules/aac_adts_analyzer_test.cpp:520-1790` (ADR-0094 9 test cases with 113–122 ordered child nodes, bit offsets, and values). | None. 100% complete bit-level coverage established. |
| **3. Stream Truncation** | `tests/rules/aac_adts_analyzer_test.cpp:321-410` (`handlesPayloadTruncationAtEof`, `handlesHeaderTruncationAtEofWithCrc`, `handlesTrailingGarbageSmallerThanHeader`). | None. Truncation isolation and source ranges fully verified; assertions will be migrated in T18c. |
| **4. CRC Presence / Errors** | `tests/rules/aac_adts_analyzer_test.cpp:185-227` (`decodesAdtsMultiFrameStream` frame 1), `:204-228` (`decodesAdtsFrameWithCrc`), `:383-410` (`handlesHeaderTruncationAtEofWithCrc`). | Bit-offset check on `crc_check` (bit 56..71) to be explicitly asserted in T18e. |
| **5. Unsupported Profiles** | `tests/rules/aac_adts_analyzer_test.cpp:106-138` (`decodesSingleAdtsFrame` LC profile value assertion), ADR-0094 §3:315 (ASC non-GA AOT parsing). | Explicit negative test rejecting non-standard ADTS profile values (e.g. profile=4) via `@enum(AacProfile)`. |

### 7. Phase 4 Task Slicing and Discipline

To uphold strict single-responsibility commits and keep capability changes separate from rule consumption:

- **Task T18a** (Current): Probing conclusions, bilingual ADR-0095, implementation plan update (Markdown-only).
- **Task T18b**: Analyzer view mapping update to frame span (`src/rules/aac_adts_analyzer.cpp:346`), runner capability slice without package version bump.
- **Task T18c**: Rule consumption of `@lazy raw_data_block` in `aac_adts.svfmt`, package version bump to `0.1.3`, test suite updates.
- **Task T18d**: Profile handling verification & documentation alignment.
- **Task T18e**: Bit-by-bit audit closing all gaps in Category 1, 4, and 5, Phase 4 checkbox completion (`docs/implementation-plan.md:196-198`), Phase advancement to Phase 5.

---

## Verification Matrix and Reproduction Steps (B4)

The verification evidence below was generated using standalone, self-contained C++ probes compiled against the `dev` preset static libraries.

**Reproduction Command**:
```bash
clang++ -std=c++20     -Isrc/rules/include -Isrc/core/include     -I/opt/homebrew/lib/QtCore.framework/Headers     -iframework /opt/homebrew/lib -F/opt/homebrew/lib -framework QtCore     build/dev/src/rules/libstreamview_rules.a build/dev/src/core/libstreamview_core.a     <probe_source.cpp> -o /tmp/probe_bin && /tmp/probe_bin
```

| Probe / Item | Source Setup / Input | Observed Output | Conclusion |
| :--- | :--- | :--- | :--- |
| **P1: DSL Syntax Check** | Grammar with `minimum_frame_length` reuse and `@lazy` payload | `Rule OK` (rc=0) | Valid syntax; no DSL compiler changes required. |
| **P2: 20-byte Frame Full Mapping (without CRC)** | 20-byte ADTS frame (7-byte header + 13-byte payload), 160-bit mapping | `status=0 materialized=1, AdtsHeader state=6 children=18, child=raw_data_block state=0 bitOffset=56 bitLength=104` | Full frame view decodes 18 children with lazy payload at bit 56. |
| **P3: 20-byte Frame Full Mapping (with CRC)** | 20-byte ADTS frame (9-byte header + 11-byte payload), 160-bit mapping | `status=0 materialized=1, AdtsHeader state=6 children=19, child=crc_check state=6 bitOffset=56 bitLength=16, child=raw_data_block state=0 bitOffset=72 bitLength=88` | Frame with CRC decodes 19 children with lazy payload at bit 72. |
| **P4: 15-byte Truncated Frame** | 20-byte declared ADTS frame cut to 15 bytes (120 bits mapping) | `status=1 materialized=0, AdtsHeader state=5 diags=1: code=0 sev=2 msg="Lazy byte region exceeds the available source range"` | Truncated payload triggers VM Error-level TruncatedSource. |
| **P5: 7-byte Header-only View** | 20-byte ADTS frame evaluated against 56-bit header mapping | `status=1 materialized=0, AdtsHeader state=5 diags=1: code=0 sev=2 msg="Lazy byte region exceeds the available source range"` | Confirms current runner header-only view mapping is a blocker. |
| **P6: T18b View Mapping No-Op** | Current official `AdtsHeader` (without lazy) against 56-bit, 160-bit, and 120-bit views | All three views output: `status=0 materialized=1` | Expanding view mapping in T18b is a safe no-op on existing rules. |
| **P7: Duplicate @range Gate (N3)** | `bits<5> aot @range(0, 4) @range(23, 23);` | `compiler diag code=14 msg="@range may appear at most once on a field"` | Disproves multi-range syntax on a single field. |
| **P8: Multi-arg @range Gate (N3)** | `bits<5> aot @range(2, 5, 29, 39);` | `parser diag code=14 msg="@range requires two integer arguments"` | Confirms `@range` cannot accept non-contiguous sets. |
| **P9: Misspelled Annotation Check (N2)** | `bits<12> syncword @equalss(4095);` on invalid stream (`syncword=255`) | Misspelled: `status=0 materialized=1 diags=0`<br>Correct `@equals`: `status=2 materialized=0 diags=1 msg="Field value violates @equals constraint"` | Demonstrates silent ignore of unknown annotations. |

---

## References

- ISO/IEC 14496-3:2019, Information technology — Coding of audio-visual objects — Part 3: Audio (Subpart 1 Annex 1.A, Subpart 4 subclauses 4.4.1, 4.4.1.1, 4.5.2.1).
- [ADR-0040: Non-Fatal Syntax Warnings and Range Annotations](0040-non-fatal-syntax-warnings-and-range-annotations.md)
- [ADR-0092: AAC ADTS Frame Enumeration and Rule Package Architecture](0092-aac-adts-frame-enumeration-and-rule-package.md)
- [ADR-0093: ADTS Header Structured Decoding and Official Rule Package](0093-adts-header-structured-decoding-and-official-rule-package.md)
- [ADR-0094: AudioSpecificConfig and Program Config Element Structured Decoding](0094-audio-specific-config-and-program-config-element.md)
