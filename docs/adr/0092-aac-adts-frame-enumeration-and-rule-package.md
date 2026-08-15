# ADR-0092: AAC ADTS Frame Enumeration Mechanism and Official Rule Package

## Status

Proposed

## Context

Phase 4 of the StreamView implementation plan introduces formal structural support for AAC-LC audio (ISO/IEC 14496-3:2019, Edition 5). The primary transport format for raw AAC audio streams is the Audio Data Transport Stream (ADTS), specified in ISO/IEC 14496-3 subclause 1.6.2.

An initial probe of the format rules language and execution pipeline revealed a critical architectural gate:
1. **DSL Scanner Closed Set**: In `src/rules/dsl.cpp:3453-3457` and `src/rules/dsl_ir.cpp:3568-3574`, sequence scan statements (`sequence<T> seq = scan(scanner_name)`) strictly permit only `scanner_name == "h264_start_code"`, rejecting any other scanner with `DslDiagnosticCode::UnsupportedScanner: "Only h264_start_code is supported"`.
2. **Framing Paradigm Divergence**:
   - **H.264 Annex B**: Delimited by 3-byte or 4-byte start codes (`0x000001` / `0x00000001`). Because NAL unit lengths are not declared in headers, NAL unit boundaries are discovered strictly via forward linear search for the subsequent start code.
   - **AAC ADTS**: Delimited by a 12-bit synchronization word `0xFFF` (byte 0: `0xFF`, byte 1 high nibble: `0xF0`). Crucially, the 7-byte (or 9-byte if CRC is present) ADTS header carries an explicit 13-bit `aac_frame_length` field (ISO/IEC 14496-3 subclause 1.6.2.1), declaring the exact total byte length $L \in [7, 8192]$ of the entire frame (headers, CRC check, and raw data blocks).
3. **Session & Runner Coupling**:
   `streamview::app::AnalysisSession` (`src/app/analysis_session.cpp:138-144`) is currently hardcoded to invoke `rules::detectH264AnnexBCandidate` and instantiate `rules::H264AnnexBAnalyzer`. Supporting AAC ADTS requires generalized format candidate detection, rule catalog selection, and an ADTS analyzer runner.

## Decision

We introduce the ADTS frame enumeration architecture, extending the DSL grammar, type system, core scanner pipeline, and official rule package catalog:

### 1. ADTS Frame Enumeration & Stepping Model

ADTS stream parsing employs a hybrid length-chain stepping and resynchronization state machine:

1. **Length-Chain Stepping (Fast Path, $O(1)$ per frame)**:
   - At candidate byte offset $P$, read the 7-byte fixed and variable header.
   - Extract $L = \text{aac\_frame\_length}$.
   - Define frame boundary as source span $[P, P + L)$.
   - Predict next frame start position at $P' = P + L$.
   - At $P'$, inspect the initial 2 bytes: verify that `(byte0 == 0xFF) && ((byte1 & 0xF6) == 0xF0)` (syncword `0xFFF`, `layer == 0`).
   - When verified, advance directly to $P'$ without linear byte scanning.
2. **Resynchronization State Machine (Slow Path)**:
   - If the syncword or header invariants at $P'$ are invalid, or upon stream initialization:
     - Search forward byte-by-byte for candidate pattern `0xFF` followed by `0xF?` (`layer == 0`).
     - Perform a 2-frame lookahead verification: extract candidate $L_0$, check offset $P + L_0$ for candidate $L_1$, and verify $P + L_0 + L_1$.
     - Upon confirmation, lock synchronization and resume length-chain stepping.
3. **Truncation & Error Isolation**:
   - If $P + L > \text{source\_size}$, the frame is materialized as a partial/truncated frame with a source-anchored diagnostic.
   - CRC errors (when `protection_absent == 0`) and out-of-range field values are published as non-fatal diagnostics without breaking length-chain progression.

### 2. Language & IR Extensions

1. **Scanner Identifier**:
   - Admit `adts_frame` as a valid scanner identifier in `scan(...)` expressions (`src/rules/dsl.cpp`).
   - Extend `DslScannerKind` in `src/rules/include/streamview/rules/dsl_ir.h`:
     ```cpp
     enum class DslScannerKind : quint8 {
         H264StartCode,
         AacAdtsFrame,
     };
     ```
   - Lower `scan(adts_frame)` to `DslTypedScan` with `scanner = DslScannerKind::AacAdtsFrame` (`src/rules/dsl_ir.cpp`).
2. **Sequence Declaration in ADTS Rules**:
   ```svfmt
   @index(progressive)
   sequence<AdtsFrame> frames = scan(adts_frame);
   entry frames;
   ```

### 3. Core Scanner & Detection Pipeline

1. **`AacAdtsScanner`**:
   Implement `AacAdtsScanner` (`src/rules/include/streamview/rules/aac_adts_scanner.h`, `src/rules/aac_adts_scanner.cpp`) producing `AacAdtsRecord` batches with exact `frameSpan`, `headerSpan`, `payloadSpan`, `crcPresent`, and `aacFrameLength`.
2. **`AacAdtsDetector`**:
   Implement `detectAacAdtsCandidate` (`src/rules/include/streamview/rules/aac_adts_detector.h`, `src/rules/aac_adts_detector.cpp`) probing the initial 64 KiB page for valid syncword chains with confidence scoring.
3. **Polymorphic Format Selection**:
   Extend `AnalysisSession` / `SessionDocument` to evaluate candidate format detectors against available rule packages in `RulePackageCatalog`, instantiating the appropriate format analyzer runner (`H264AnnexBAnalyzer` or `AacAdtsAnalyzer`).

### 4. Official AAC Rule Package (`org.streamview.aac`)

1. **Manifest** (`src/rules/official/org.streamview.aac/rule.toml`):
   ```toml
   manifest-version = 1

   [package]
   id = "org.streamview.aac"
   version = "0.1.0"
   authors = ["StreamView contributors"]
   license = "MIT"
   dependencies = []

   [compatibility]
   language = "0.1"
   engine = ">=0.1.0 <0.2.0"

   [[entrypoints]]
   id = "adts"
   format = "audio.aac.adts"
   source = "src/aac_adts.svfmt"
   profiles = ["lc"]
   depth = "adts-frame"
   detector = "aac-adts"
   ```
2. **Normative References**:
   All fields and enumerations are referenced to ISO/IEC 14496-3:2019 (Edition 5):
   - Table 1.11 — `AacProfile` (MPEG-4 Audio Object Types minus 1: `0` Main, `1` LC, `2` SSR)
   - Table 1.16 — `AacSamplingFrequencyIndex` (`0` 96000 Hz .. `12` 7350 Hz, `15` explicit)
   - Table 1.17 — `AacChannelConfiguration` (`0` Custom/PCE, `1` Mono, `2` Stereo, `3` 3-channel, `4` 4-channel, `5` 5-channel, `6` 5.1, `7` 7.1)
   - Subclause 1.6.2.1 — `adts_fixed_header`, `adts_variable_header`
   - Subclause 1.6.2.2 — `adts_error_check` (`crc_check`)

### 5. Boundary Contracts for First Increments

1. **Single Raw Data Block**: `number_of_raw_data_blocks_in_frame == 0` (1 raw data block per frame). Multiple raw data blocks with inter-block 16-bit CRC headers (`raw_data_block_position`) are explicitly postponed.
2. **Value Domain Classification**:
   - `sampling_frequency_index == 15`: Escape value forbidden in ADTS per ISO/IEC 14496-3 subclause 1.6.2.1; classified as non-fatal warning/error.
   - `channel_configuration == 0`: Indicates program configuration element (PCE) in raw data block; parsed in Task T17.
   - `adts_buffer_fullness == 0x7FF`: Valid special value denoting variable bit rate (VBR) stream.
3. **Source-Anchored Assertion**:
   Enforce `assert(aac_frame_length >= (protection_absent == 1 ? 7 : 9))` to guard against corrupted frame length fields smaller than header size.

## Phased Implementation Sequence

- **Task T14 (Current)**: Architectural probe report and bilingual ADR-0092 specification (Markdown-only).
- **Task T15**: ADTS enumeration capability slice (`DslScannerKind::AacAdtsFrame`, `AacAdtsScanner`, `dsl.cpp`, `dsl_ir.cpp`, targeted unit tests, capability-only).
- **Task T16**: Rule package creation (`org.streamview.aac` v0.1.0) and ADTS header structured decoding (ADR-0093).
- **Task T17**: AudioSpecificConfig (ASC) and GASpecificConfig structured decoding (v0.1.1, ADR-0094).
- **Task T18**: Program Config Element (PCE) and unsupported profile diagnostics (v0.1.2, ADR-0095).

## References

- ISO/IEC 14496-3:2019, Edition 5, Subclauses 1.6.2.1, 1.6.2.2, Tables 1.11, 1.16, 1.17
- ADR-0010: C-Style Declarative Format Description Language
- ADR-0016: TOML Manifest And ZIP Rule Packages
- ADR-0027: Resume Cancelled Progressive H.264 Indexes In Place
- ADR-0030: Canonical Rule Package Identity and Catalog
