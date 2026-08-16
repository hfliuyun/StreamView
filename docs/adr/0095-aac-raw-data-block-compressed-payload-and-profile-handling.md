# ADR-0095: AAC Raw Data Block Compressed Payload and Profile Handling

- **Status**: Proposed
- **Date**: 2026-08-16
- **Authors**: StreamView Contributors
- **Area**: DSL Rules / AAC Decoding / Analyzer Execution

## Context

Phase 4 of the StreamView implementation plan specifies full structural support for AAC-LC audio (ISO/IEC 14496-3:2019, Edition 5). Following the completion of ADTS header structured decoding (ADR-0093, Task T16) and AudioSpecificConfig / Program Config Element decoding (ADR-0094, Task T17c), Phase 4 requires completing three final milestones (`docs/implementation-plan.md:206-208`):
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
   - `@enum` violation is fatal (`DiagnosticCode::InvalidSyntax`, Severity: Error, `src/rules/dsl_vm.cpp:2782-2790`), which would reject frame decoding rather than non-fatally reporting unsupported capability.
   - Non-contiguous value sets (e.g. AOT 2, 5, 29, 39) cannot be expressed via `@range`: `@range` requires exactly two integer arguments (`src/rules/dsl.cpp:2560` / `src/rules/dsl_ir.cpp:185`) and duplicate `@range` annotations on a single field are rejected (`src/rules/dsl.cpp:2534` / `src/rules/dsl_ir.cpp:173`, `@range may appear at most once on a field`).
   - In ADTS headers, the 2-bit `profile` field (ISO/IEC 14496-3 Table 1.A.1) only encodes 0..3 (Main, LC, SSR, LTP); it cannot represent AOT 5 (SBR), 29 (PS), or 39 (ELD). In standard broadcast streams, HE-AAC streams in ADTS declare `profile = 1` (LC) in the ADTS header and convey SBR/PS extensions in-band inside `raw_data_block`.

6. **Silent Ignorance of Unrecognized Annotations (N2)**:
   Probing revealed that `DslCompiler` (`src/rules/dsl_ir.cpp`) validates known annotations (`@equals`, `@range`, `@enum`, `@spec`, `@description`, `@context_export`) but silently ignores unrecognized annotations on declared bit fields. For instance, `bits<12> syncword @equalss(4095);` compiles with `Rule OK`, and executing against an invalid stream (`syncword=255`) yields `status=Materialized`, `materialized=1`, `diags=0`, whereas `@equals(4095)` correctly emits `InvalidSyntax` / `Field value violates @equals constraint`. This behavior is recorded as an explicit observation and escalated for future language gate hardening.

---

## Decision

### 1. Specification Citations for `raw_data_block` (B2, C4)

In ISO/IEC 14496-3:2019 (Edition 5):
- **ADTS Payload Sequencing**: Subpart 1 Annex 1.A subclause **1.A.1** (*Fixed and variable header of ADTS*) and Table 1.A.5 (`adts_frame()`), where `adts_frame()` sequences `adts_fixed_header()`, `adts_variable_header()`, optional `adts_error_check()`, followed by `raw_data_block()`.
- **Raw Data Block Syntax Definition**: Subpart 4 (General Audio) subclause **4.5.2.1** (*raw_data_block* / *Syntactic elements*), which standardizes the grammatical element composition of `raw_data_block()` (SCE, CPE, LFE, DSE, PCE, FIL, TERM).

*Normative Verification Note*: As the repository does not bundle normative ISO/IEC standard body texts (`docs/standards.md:3-4`), the specific subclause number `4.5.2.1` and Table `1.A.5` are derived from ISO/IEC 14496-3 structural hierarchy and secondary MPEG-4 Audio references, but are explicitly marked as unverified against the physical standard text. The confirmed minimum normative scope is Subpart 1 Annex 1.A (ADTS transport framing) and Subpart 4 (General Audio syntax).

To maintain strict syntactic decoupling, `@spec("ISO/IEC 14496-3:2019", "4.5.2.1")` is assigned to `raw_data_block`, distinguishing the payload's syntax definition from the `1.A.1` fixed/variable header clauses.

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

### 4. Harmonized Truncation Contract and Test Migration (C1, C2, N5)

We adopt the universal DSL VM truncation contract (Option A):
- When a stream ends prematurely in the middle of a frame's payload, the DSL VM attempts to allocate `raw_data_block_bytes` and detects that `bitCount > reader.remainingBits()`.
- The DSL VM emits `DslExecutionStatus::TruncatedSource` with Error severity and message `Lazy byte region exceeds the available source range`.
- The frame node is marked as `Invalid`, retaining its decoded header field children and source-anchored location.
- The legacy C++ synthetic warning at `aac_adts_analyzer.cpp:456-467` is superseded by this native VM diagnostic.

**Test Suite Migrations required in Task T18c**:
- `tests/rules/aac_adts_analyzer_test.cpp:141-187` (Frame 0 in `createsAnalyzerFromBundledPackageAndDecodesFieldsViaDsl`): `children().size()` at line 144 advances from 16 to 18; `expectedNames0` appends `raw_data_block_bytes` and `raw_data_block`.
- `tests/rules/aac_adts_analyzer_test.cpp:189-230` (Frame 1 in `createsAnalyzerFromBundledPackageAndDecodesFieldsViaDsl`): `children().size()` at line 196 advances from 17 to 19; `expectedNames1` appends `raw_data_block_bytes` and `raw_data_block`.
- `tests/rules/aac_adts_analyzer_test.cpp:325-365` (`handlesPayloadTruncationAtEof`):
  - Line 356: `DiagnosticSeverity::Warning` $\to$ `DiagnosticSeverity::Error`;
  - Line 357: `"ADTS frame payload is truncated at EOF"` $\to$ `"Lazy byte region exceeds the available source range"`;
  - Line 364: `header1->state() == MaterializationState::Materialized` $\to$ `header1->state() == MaterializationState::Invalid`;
  - Line 360: `node1->children().size() == 1` remains unchanged.
- `tests/rules/aac_adts_analyzer_test.cpp:602` (`resolvesAscEntryPointFromBundledRulePackage`): `loaded.package->identity().packageVersion()` assertion updates from `"0.1.2"` to `"0.1.3"`.

*Dead Code Removal, Qualified Reachability Rationale & Behavior Narrowing (G3, Task T18c-2)*:
In `src/rules/aac_adts_analyzer.cpp`, the legacy synthetic warning branch for payload truncation (`if (record.truncated) { ... }`) was removed.

1. **Qualified Reachability Proof (Official Package)**: The three-path reachability proof strictly holds under the prerequisite that the active rule declares a `@lazy` payload region covering the remaining frame bytes (as fulfilled by the official `org.streamview.aac` 0.1.3 package):
   - **Path A (DSL Execution Failure Isolation)**: In `publishRecord`, when DSL decoding fails (`!execution.materialized()`), line 426 marks `frameNode` as `Invalid` with the VM diagnostic, appends to `batch.frameNodes`, and line 452 immediately returns `true` (or returns `false` for fatal errors), never falling through to downstream statements.
   - **Path B (Mathematical Guarantee of VM Truncation)**: Truncation occurs when `availableBytes < aac_frame_length` (`src/rules/aac_adts_scanner.cpp:143`). Since `raw_data_block_bytes = aac_frame_length - minimum_frame_length`, after consuming `minimum_frame_length` bytes, remaining available reader bytes are `availableBytes - minimum_frame_length < raw_data_block_bytes`. Thus, `bitCount > reader.remainingBits()` holds unconditionally in `src/rules/dsl_vm.cpp:3140`, returning `DslExecutionStatus::TruncatedSource` (`"Lazy byte region exceeds the available source range"`), which triggers Path A. (Boundary note: when `availableBytes == aac_frame_length`, `record.truncated` is false per scanner line 140).
   - **Path C (Guard Invariant)**: In `AacAdtsScanner`, any published record requires `offset + 7 <= sourceSize` (`src/rules/aac_adts_scanner.cpp:57-65`), guaranteeing `availableHeader = min(availableBytes, headerLength) >= 7` bytes (`src/rules/aac_adts_scanner.cpp:144`). Thus `record.headerSpan->bitLength() >= 56 > 0` always holds, proving that the guard `record.headerSpan && record.headerSpan->bitLength() > 0` at line 329 can never short-circuit.
2. **Behavior Narrowing on Custom / Third-Party Rules**: For rule packages matching `audio.aac.adts` format that do not declare a payload region (admitted via `src/app/analysis_session.cpp:144-147`), a truncated trailing frame is decoded successfully by the DSL VM for its header fields and transitions to `Materialized` with 0 diagnostics, with its logical range clamped to available bytes. This known capability boundary is explicitly pinned by regression test `materializesTruncatedTrailingFrameWhenRuleLacksPayloadDeclaration` (`tests/rules/aac_adts_analyzer_test.cpp:379-475`).
3. **Architectural Rationale**: Synthesizing format-specific diagnostic messages in C++ violates the core architectural rule that format semantics belong exclusively in DSL/rule layers. Proper payload truncation reporting is governed by the universal VM truncation contract upon evaluating declared lazy regions (ADR-0092 §1.3).
4. **Status of `record.truncated`**: `record.truncated` in `src/` now has zero reading points in the analyzer runner; it is retained as a scanner-level observation fact and continuously asserted by the scanner test suite (`tests/rules/aac_adts_scanner_test.cpp`).

### 5. Profile Handling and Explicit Capability Boundaries

1. **ADTS Transport Streams**: Constrained to `AacProfile` (0=Main, 1=LC, 2=SSR, 3=LTP) via `@enum(AacProfile)`. HE-AAC v1/v2 streams transmitted via ADTS carry `profile = 1` (LC) in the ADTS header per broadcast standards, decoding successfully as LC frames with unparsed raw data payloads.
2. **AudioSpecificConfig (ASC)**: `audio_object_type` values outside General Audio (e.g. SBR=5, PS=29, ELD=39) decode the GA baseline header syntax without syntax failure (`MaterializationState::Materialized`), as established in ADR-0094 §3:315. This is formally accepted as a documented capability boundary until dedicated non-GA payload decoders are introduced in future milestones.
3. **ADTS Rules Lacking Payload Region Declarations**: Rule packages matching `audio.aac.adts` format that only declare header fields without `@lazy` payload regions cannot emit payload truncation diagnostics. Truncated trailing frames will materialize as valid with zero diagnostics, with their bit length clamped to available source bytes. Any future universal detection of region-level truncation must be achieved through format-neutral core mechanisms (e.g. marking the container region node as partially materialized with a generic truncation diagnostic when the scanner flags truncation) rather than format-specific C++ string synthesis.
4. **Phase 4 Item 4 Formal Closure Decision (Bounded by Capability Baseline)**: StreamView v0.1 does not provide a non-fatal DSL/core mechanism to report 'unsupported' or 'partially identified' profiles without failing execution. ADTS streams with standard profiles 0..3 and ASC configurations for GA / non-GA AOTs (including HE-AAC SBR=5, PS=29, and AAC-ELD=39) are parsed up to their declared baseline syntax and materialized without error. Dedicated non-fatal unsupported diagnostics require future core and language capabilities (e.g. `@unsupported` annotation). Phase 4 Item 4 is thus formally closed as a documented and tested capability boundary.

### 6. Bit-by-Bit Acceptance Audit Scope and Coverage Matrix (B1, C1, C3)

Phase 4 Item 5 (`docs/implementation-plan.md:208`) requires bit-by-bit verification across five distinct categories.

#### Definition of "CRC Error / Presence" Acceptance Boundary
StreamView rules do NOT perform CRC-16 polynomial division or arithmetic checksum calculation (just as Huffman decoding is not performed in DSL). The acceptance criteria for CRC error / presence are bounded to:
1. `protection_absent == 0`: The 16-bit `crc_check` field is materialized at exact bit offset 56..71 (bytes 7..8).
2. `protection_absent == 1`: `crc_check` is omitted; `raw_data_block` payload starts at exact bit offset 56 (byte 7).
3. `protection_absent == 0` with frame length < 9 bytes: Emits `TruncatedSource` diagnostic without crashing.

#### Coverage and Gap Inventory

| Category | Final Verified Coverage (`file:line`) | Audit Status and Verification Scope |
| :--- | :--- | :--- |
| **1. ADTS Headers** | `tests/rules/aac_adts_analyzer_test.cpp:106-231` (`createsAnalyzerFromBundledPackageAndDecodesFieldsViaDsl`), `:2473-2655` (`decodesAdtsHeaderBitByBitRangesAndZeroLengthPayload`). | **Closed**: Full bit-by-bit coverage across all 18/19 fields (including indices 8–11 `original_copy`, `home`, `copyright_identification_bit`, `copyright_identification_start`), bit offsets 0..55/71, lengths, values, zero-length payload frames (`raw_data_block` state=Materialized, length=0, 0 diagnostics), and non-zero payload frames (`raw_data_block` state=Lazy at :2600 [Frame 2, offset 56, len 744] and :2652 [Frame 3, offset 72, len 728]). |
| **2. ASC / PCE** | `tests/rules/aac_adts_analyzer_test.cpp:629-1865` (`decodesAscCase1`..`7`, `rejectsAscCase8NonzeroAlignmentBit`, `rejectsAscCase9PrematureTruncation`), `:1960-2467` (AOT 5/29/39 non-GA suites), `:2661-2732` (`decodesAscFieldRangesRepresentativeSampling`). | **Closed (Representative Sampling)**: 9 full ASC test suites verifying 113–122 ordered child node names; representative field range and bit length assertions covering ASC header (`audio_object_type` [0,5), `sampling_frequency_index` [5,9), `channel_configuration` [9,13)), GASpecificConfig (`frame_length_flag` [13,14), `depends_on_core_coder` [14,15), `extension_flag` [15,16)), and PCE (`element_instance_tag` [16,20), `object_type` [20,22), `comment_field_bytes` [56,64)). **Explicitly Unsampled Remainder**: PCE scalar fields (`pce_sampling_frequency_index`, `num_front_channel_elements`, `num_side_channel_elements`, `num_back_channel_elements`, `num_lfe_channel_elements`, `num_assoc_data_elements`, `num_valid_cc_elements`, `mono_mixdown_present`, `stereo_mixdown_present`, `matrix_mixdown_idx_present`, `pce_alignment_bits`) and repetitive per-channel element list entries. |
| **3. Stream Truncation** | `tests/rules/aac_adts_analyzer_test.cpp:290-323` (`handlesHeaderTruncationWithCrcPresent`), `:325-365` (`handlesPayloadTruncationAtEof`), `:379-475` (`materializesTruncatedTrailingFrameWhenRuleLacksPayloadDeclaration`), `:477-500` (`handlesTrailingGarbageSmallerThanHeader`), `:502-530` (`resynchronizesAcrossCorruptedByteSpan`), `:2737-2795` (`verifiesTruncatedFramesLogicalRangesAndDiagnosticLocations`). | **Closed**: Explicit `logicalRange()` assertions on truncated frame nodes ([0, 120) for 15-byte payload truncation, [0, 64) for 8-byte CRC header truncation) and diagnostic locations ([56, 120) for payload truncation, [56, 64) for header truncation). |
| **4. CRC Presence / Errors** | `tests/rules/aac_adts_analyzer_test.cpp:189-230` (`createsAnalyzerFromBundledPackageAndDecodesFieldsViaDsl` Frame 1), `:290-323` (`handlesHeaderTruncationWithCrcPresent`), `:2473-2655` (`decodesAdtsHeaderBitByBitRangesAndZeroLengthPayload`), `:2737-2795` (`verifiesTruncatedFramesLogicalRangesAndDiagnosticLocations`). | **Closed**: Explicit verification of `crc_check` field at bit 56..71 (16 bits, value `0x1234`), omitted when `protection_absent == 1`, and truncated header handling at bit 56..64 when CRC is incomplete. |
| **5. Unsupported Profiles** | `tests/rules/aac_adts_analyzer_test.cpp:106-231` (`createsAnalyzerFromBundledPackageAndDecodesFieldsViaDsl`), `:1916-1958` (`decodesAllStandardAdtsProfilesMainLcSsrLtp`), `:1960-2128` (`decodesAscNonGaAot5Sbr`), `:2130-2298` (`decodesAscNonGaAot29ParametricStereo`), `:2300-2467` (`decodesAscNonGaAot39EnhancedLowDelay`). | **Closed (Capability Boundary)**: Standard ADTS profiles 0..3 clean AST materialization pinned; non-GA AOTs 5, 29, 39 parsing baseline GA headers cleanly pinned (AOT 5 and 29 bit layout identical to AOT 2; closed via §5 Item 4). |

### 7. Phase 4 Task Slicing and Discipline

To uphold strict single-responsibility commits and keep capability changes separate from rule consumption:

- **Task T18a**: Probing conclusions, bilingual ADR-0095, implementation plan update (Markdown-only).
- **Task T18b**: Analyzer view mapping update to frame span (`src/rules/aac_adts_analyzer.cpp:346`), runner capability slice without package version bump.
- **Task T18c**: Rule consumption of `@lazy raw_data_block` in `aac_adts.svfmt`, package version bump to `0.1.3`, test suite updates.
- **Task T18c-2**: Dead code removal in `src/rules/aac_adts_analyzer.cpp:456-467` with reachability rationale (runner capability slice, no version bump).
- **Task T18d**: Profile handling verification & documentation alignment.
- **Task T18e**: Bit-by-bit audit closing all gaps in Category 1, 2, 3, 4, and 5, Phase 4 checkbox completion (`docs/implementation-plan.md:206-208`), Phase advancement to Phase 5.
- **Task T18f**: Phase 4 wrap-up remediation: markdown hygiene line calculations, Frame 2/3 Lazy state assertions, and PCE unsampled scalar margin declaration.

#### Anti-Recurrence Discipline for Test Citations
To prevent documentation line citation drift when modifying test suites:
1. **Append-by-Default**: New test methods must be appended to the end of the test class by default (after the last existing test case). If middle insertion is strictly required, all affected downstream document line number citations across all ADRs and implementation plans must be audited and updated within the same commit.
2. **Same-Commit Line Audit**: Any commit causing line number displacement in referenced files (including `tests/` and `docs/` themselves) must run `grep -n` against all documentation citations and reconcile all drifted line numbers in the same commit.

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
| **P7: Duplicate @range Dual Gate (N3)** | `bits<5> aot @range(0, 4) @range(23, 23);` | `diag code=14 msg="@range may appear at most once on a field"` | Disproves multi-range syntax on a single field (`src/rules/dsl.cpp:2534` / `src/rules/dsl_ir.cpp:173`). |
| **P8: Multi-arg @range Dual Gate (N3)** | `bits<5> aot @range(2, 5, 29, 39);` | `diag code=14 msg="@range requires two integer arguments"` | Confirms `@range` cannot accept non-contiguous sets (`src/rules/dsl.cpp:2560` / `src/rules/dsl_ir.cpp:185`). |
| **P9: Misspelled Annotation Check (N2)** | `bits<12> syncword @equalss(4095);` on invalid stream (`syncword=255`) | Misspelled: `status=0 materialized=1 diags=0`<br>Correct `@equals`: `status=2 materialized=0 diags=1 msg="Field value violates @equals constraint"` | Demonstrates silent ignore of unknown annotations. |

---

## References

- ISO/IEC 14496-3:2019, Information technology — Coding of audio-visual objects — Part 3: Audio (Subpart 1 Annex 1.A, Subpart 4 subclauses 4.4.1, 4.4.1.1, 4.5.2.1).
- [ADR-0040: Non-Fatal Syntax Warnings and Range Annotations](0040-non-fatal-syntax-warnings-and-range-annotations.md)
- [ADR-0092: AAC ADTS Frame Enumeration and Rule Package Architecture](0092-aac-adts-frame-enumeration-and-rule-package.md)
- [ADR-0093: ADTS Header Structured Decoding and Official Rule Package](0093-adts-header-structured-decoding-and-official-rule-package.md)
- [ADR-0094: AudioSpecificConfig and Program Config Element Structured Decoding](0094-audio-specific-config-and-program-config-element.md)
