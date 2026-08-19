# ADR-0105: Container Sample Navigation, Timeline Indexing, and Access Unit Execution Contracts

- **Status**: Proposed
- **Date**: 2026-08-19
- **Authors**: StreamView Contributors

---

## Context

Phase 5 of StreamView delivers non-fragmented ISO BMFF MP4/MOV container inspection, metadata tree materialization, large `mdat` lazy encapsulation, sample table windowed paging, and cross-layer navigation to elementary streams.

Tasks P5a through P5i successfully realized:
1. Top-level box scanning and lazy `mdat` encapsulation (Task P5e, `org.streamview.mp4` v0.1.0);
2. `moov` container hierarchy and time headers (Task P5f, `org.streamview.mp4` v0.1.1);
3. Windowed sample table entry parsing for `stts`, `stsc`, `stsz`, `stco`, and `co64` (Task P5g, `org.streamview.mp4` v0.1.2);
4. Sample description and codec configuration schemas for `stsd`, `avc1`, `avcC`, `mp4a`, and `esds` (Task P5h, `org.streamview.mp4` v0.1.3);
5. Cross-layer navigation from `avcC` (SPS/PPS) and `esds` (ASC) into `video.h264.nal` and `audio.aac.asc` sub-trees with session context sharing, breadcrumbs, and bidirectional coordinate highlighting (Task P5i, ADR-0103 / ADR-0104).

### Problem Statement & Gap Audit

While Task P5i achieved cross-layer navigation for stationary codec configuration payloads (`avcC` / `esds`), the StreamView Product Requirements ([PRD](../product-requirements.md) §§22, 37, 47) and Phase 5 acceptance criteria require navigating from **container media samples in `mdat`** into underlying H.264 or AAC syntax.

A comprehensive architectural audit reveals the following capability and specification gaps between Task P5i and full container sample navigation:

1. **Stationary Config vs. Media Sample Payloads**:
   - Task P5i navigated stationary configuration headers located in `moov` (`avcC` SPS/PPS and `esds` ASC).
   - Media data in `mdat` is not stored as self-describing container atoms; its physical byte locations, durations, presentation timestamps, and sync/keyframe attributes are determined by composing multiple sample tables.
2. **Missing Sample Indexing & Timeline Tables**:
   - `stss` (Sync Sample Box) is required to identify random access keyframes.
   - `ctts` (Composition Time to Sample Box) is required to compute Presentation Time Stamps ($\text{PTS} = \text{DTS} + \text{composition\_offset}$) in the presence of B-frames / temporal reordering.
   - A format-neutral composite sample indexer combining `stts`, `stsc`, `stsz`/`stz2`, `stco`/`co64`, `stss`, and `ctts` does not yet exist.
3. **Framing Mismatch for AVC Media Samples**:
   - An AVC video sample in `mdat` is NOT a single standalone NAL unit and NOT an Annex B byte stream with `00 00 01` start codes.
   - Under ISO/IEC 14496-15, an AVC sample contains one or more length-prefixed NAL units, where the prefix length is `lengthSizeMinusOne + 1` bytes (from `avcC`).
4. **AAC Audio Sample Presentation Level**:
   - An AAC sample in `mdat` is an access unit containing a single `raw_data_block`.
   - Per the PRD, spectral Huffman decoding is deferred in v0.1. The presentation level for AAC samples must be precisely bounded.
5. **Persistence Boundary Clarification**:
   - The PRD states that saved sessions retain navigation state, whereas ADR-0103 defines navigation stack frames as ephemeral in-process state in v0.1.

This ADR establishes the normative architectural decisions and slice plan for Task P5j to close all gaps and complete Phase 5.

---

## Decision

### 1. Normative Definition of "MP4 Sample" in Phase 5

In StreamView Phase 5, an **MP4 Sample** is defined as:
> A discrete logical access unit whose physical location and byte extent within the root media source (`mdat`) are derived by evaluating the track's sample tables (`stsc`, `stsz`/`stz2`, `stco`/`co64`), whose decoding and presentation timestamps are derived from `stts` and `ctts`, whose random access capability is derived from `stss`, and whose format semantics are governed by the associated `stsd` sample entry.

- **Prerequisite vs. Final Capability**: Navigating stationary codec configuration payloads (`avcC` / `esds`) in Task P5i was a prerequisite capability slice. Container sample navigation in Task P5j is the definitive completion of Phase 5 container analysis.
- **Physical Extent**: Every sample resolves to an exact `core::SourceSpan` in the root file. Zero heap data copying is permitted.

---

### 2. `SampleDescriptor` Schema and Interface

To decouple sample table indexing from UI presentation and codec execution, the engine shall define a format-neutral `SampleDescriptor` struct in `src/core/`:

```cpp
namespace streamview::core {

struct SampleDescriptor final {
    uint32_t trackId = 0;
    uint64_t sampleIndex = 0;              // 0-based sample ordinal within track
    uint32_t sampleDescriptionIndex = 1;   // 1-based index into stsd entries
    SourceSpan sourceSpan;                 // Absolute byte span in root media source
    uint64_t dts = 0;                      // Decoding Time Stamp in timescale units
    uint64_t pts = 0;                      // Presentation Time Stamp in timescale units
    uint64_t duration = 0;                 // Sample duration in timescale units
    uint32_t timescale = 1;                // Track timescale from mdhd
    bool isSyncSample = true;              // Keyframe / random access point
    QString targetFormat;                  // e.g. "video.h264.sample", "audio.aac.sample"
};

} // namespace streamview::core
```

---

### 3. `stss` and `ctts` Specification & Indexing Rules

#### 3.1 Sync Sample Box (`stss`)
- **DSL Schema**: `stss` (`0x73747373`) decodes FullBox header and `@window(SyncSampleEntry, entry_count)` where each entry contains `bits<32> sample_number;` (1-based).
- **Omission Semantics**: Per ISO/IEC 14496-12 §8.6.2.1, **if `stss` is absent in a track, every sample in that track is a sync sample (`isSyncSample = true`)**. This rule is strictly applied to AAC audio tracks and all-intra video streams.
- **Present Semantics**: When `stss` is present, only samples whose 1-based index appears in the `stss` table are marked `isSyncSample = true`; all other samples are marked `isSyncSample = false`.

#### 3.2 Composition Time to Sample Box (`ctts`)
- **DSL Schema**: `ctts` (`0x63747473`) decodes FullBox header and `@window(CompositionOffsetEntry, entry_count)`.
  - `version == 0`: entries contain `bits<32> sample_count;` and `bits<32> sample_offset;` (unsigned).
  - `version == 1`: entries contain `bits<32> sample_count;` and `bits<32> sample_offset;` (signed 32-bit two's complement for negative composition delays).
- **PTS Calculation**:
  - When `ctts` is present: $\text{PTS} = \text{DTS} + \text{sample\_offset}$.
  - When `ctts` is absent: $\text{PTS} = \text{DTS}$.
- **Truth in Timeline Claims**: Accurate verification of presentation timestamps on streams with B-frames strictly requires evaluating `ctts`.

---

### 4. AVC Sample Framing (`lengthSizeMinusOne`) & Multi-NAL Execution Contract

Under ISO/IEC 14496-15 §5.3.4.2.1, an AVC video sample in `mdat` consists of one or more length-prefixed NAL units:

$$\underbrace{[\text{Length}]_{L\text{ bytes}}[\text{NAL Unit}]}_{\text{NAL } 1}\;\underbrace{[\text{Length}]_{L\text{ bytes}}[\text{NAL Unit}]}_{\text{NAL } 2}\;\dots$$

where $L = \text{lengthSizeMinusOne} + 1 \in \{1, 2, 4\}$ (extracted from the track's `avcC` configuration record, typically $L = 4$).

#### Execution Rules:
1. **Length-Prefixed Framing**: The sample runner parses the length prefix $L$, validates that the indicated NAL byte length fits within the sample boundary, and creates child `SourceMapping` spans for each contained NAL unit.
2. **Multi-NAL Aggregation**: A single video sample may contain multiple NAL units (e.g. AUD, SEI, primary slice, redundant slice). The sample execution produces a structured sub-tree containing all NAL units in sequence.
3. **Session Context Sharing**: Each contained NAL unit is executed as `NalUnitHeader` + payload, inheriting the session's active SPS/PPS parameter set context established during `avcC` inspection or preceding keyframe processing.

---

### 5. AAC Access Unit v0.1 Presentation Hierarchy

Under ISO/IEC 14496-14 and 14496-3, an audio sample in `mdat` is an AAC raw data block access unit.

Per the PRD ("AAC Huffman spectral payload decoding ... are deferred"):
1. StreamView v0.1 does NOT decode individual Huffman spectral coefficients or MDCT bins inside the sample payload.
2. Navigating into an AAC sample produces an **Access Unit Envelope** sub-tree exposing:
   - Sample metadata (sample index, DTS/PTS, duration, physical byte length);
   - Raw access unit byte span mapped directly to `mdat`;
   - Formatted reference to the track's active `AudioSpecificConfig` (sampling frequency, channels, audio object type).
3. Exact physical coordinate highlighting in `RawDataView` is preserved for the entire access unit byte span.

---

### 6. Paging, Resource Bounds, Cancellation, and Error Isolation

1. **Progressive Indexing**: For 100+ GB files containing hundreds of thousands of samples, `Mp4SampleTableIndex` is constructed on demand and cached progressively in SQLite WAL using `WindowDecoder` and `CancellationToken`.
2. **Virtualized UI Presentation**: The track sample list in `MainWindow` is virtualized and paginated (e.g. 256 or 1000 samples per page), keeping materialized tree nodes bounded well below `defaultMaximumMaterializedNodes() = 100,000`.
3. **On-Demand Execution**: Samples are executed only when explicitly navigated by the user, applying bounded execution limits per navigation action.
4. **Error Isolation**: A corrupt or truncated sample in `mdat` emits a `TruncatedSource` or `InvalidSyntax` diagnostic localized to that sample frame without invalidating the parent container, other samples, or session state.

---

### 7. Documentation Consistency on Navigation State & SessionDocument Schema

- **PRD Alignment**: In StreamView v0.1, `SessionDocument` strictly persists root-level session state (source identity, rule package versions, bookmarks, user annotations).
- **Navigation Stack Boundary**: The navigation stack (sub-format frames, sample frames) is an ephemeral in-process interaction state in v0.1. Full serialization of child navigation hierarchies is deferred to Phase 7 session extensions.

---

### 8. ADR-0103 Lifecycle Transition

With the full implementation and verification of Task P5i (Tasks P5i-1, P5i-2, P5i-3, P5i-4a, P5i-4a-R, P5i-4b), the architectural contracts of ADR-0103 are proven. Upon completion of Phase 5 (Task P5j), ADR-0103 shall formally transition from `Proposed` to `Accepted`.

---

## Phase 5j Implementation Slice Plan

To ensure incremental verification, isolation of concerns, and rigorous quality gates, Task P5j is decomposed into six sequential tasks:

```
[Task P5j-0 (Docs)]: Gap Audit & Architectural Decisions (ADR-0105)
      │
      ▼
[Task P5j-1 (Rules/MP4)]: stss & ctts DSL Rules in org.streamview.mp4 v0.1.4
      │
      ▼
[Task P5j-2 (Rules/Core)]: Mp4SampleTableIndex Composite Timeline & Sample Service
      │
      ▼
[Task P5j-3 (Rules/Runtime)]: AVC Length-Prefixed Multi-NAL & AAC Access Unit Execution
      │
      ▼
[Task P5j-4 (App/Core)]: AnalysisSession Sample Navigation API & Coordinate Projection
      │
      ▼
[Task P5j-5 (App/UI)]: MainWindow Track/Sample Dock, Timeline Grid & Breadcrumb Integration
      │
      ▼
[Task P5j-6 (Verification)]: Phase 5 Large-File Matrix, Reference Tool Cross-Check & Milestone Close
```

### Slice Details:

1. **Task P5j-0 (Specification & Gap Audit)**:
   - Deliverable: Bilingual ADR-0105 (Markdown-only).
   - Scope: Audits capability gaps, defines normative contracts, establishes P5j-1..P5j-6 slice plan.

2. **Task P5j-1 (DSL & Official MP4 Rule Package v0.1.4)**:
   - Deliverable: Add `stss` (`SyncSampleBox`) and `ctts` (`CompositionOffsetBox`) schemas to `mp4_isobmff.svfmt`; bump `org.streamview.mp4` to `0.1.4`.
   - Files: `src/rules/official/org.streamview.mp4/src/mp4_isobmff.svfmt`, `rule.toml`, tests in `mp4_isobmff_analyzer_test.cpp`.

3. **Task P5j-2 (Composite Sample Indexer & Timeline Service)**:
   - Deliverable: Format-neutral `Mp4SampleTableIndex` combining `stts`, `stsc`, `stsz`/`stz2`, `stco`/`co64`, `stss`, and `ctts` into `SampleDescriptor` sequences with bounded memory and cancellation support.
   - Files: `src/rules/mp4_sample_table_index.h`, `src/rules/mp4_sample_table_index.cpp`, `tests/rules/mp4_sample_table_index_test.cpp`.

4. **Task P5j-3 (AVC Length-Prefixed Multi-NAL & AAC Sample Runner)**:
   - Deliverable: Format-neutral sample payload splitter and runner handling `lengthSizeMinusOne + 1` NAL prefixes, multi-NAL aggregation, RBSP transform, and session context resolution.
   - Files: `src/rules/sample_payload_runner.h`, `src/rules/sample_payload_runner.cpp`, `tests/rules/sample_payload_runner_test.cpp`.

5. **Task P5j-4 (AnalysisSession Sample Navigation API)**:
   - Deliverable: `AnalysisSession::enterSample(trackId, sampleIndex)`, `samplesForTrack`, and sample coordinate projection to `mdat`.
   - Files: `src/app/analysis_session.h`, `src/app/analysis_session.cpp`, `tests/app/analysis_session_test.cpp`.

6. **Task P5j-5 (MainWindow Track / Sample Navigation & Timeline UI)**:
   - Deliverable: Track/Sample dock view, virtualized sample table, sync sample badges, double-click / keyboard sample navigation, breadcrumb path `video.mp4 > Track 1 (avc1) > Sample #42 [Sync] > NalUnitHeader`, and bidirectional coordinate highlighting.
   - Files: `src/app/main_window.h`, `src/app/main_window.cpp`, `tests/app/main_window_test.cpp`.

7. **Task P5j-6 (Phase 5 Milestone Verification & Closure)**:
   - Deliverable: Large-file 100 GB virtual sparse verification, reference tool cross-validation (`ffprobe`/`mediainfo`), ADR-0103 and ADR-0105 lifecycle state transition to `Accepted`, Phase 5 completion signoff in `docs/implementation-plan.md`.

---

## Consequences

### Positive
- Closes the final capability gap for Phase 5, delivering end-to-end container sample navigation from container down to codec fields.
- Provides mathematically exact DTS and PTS timeline calculation with full B-frame support via `ctts`.
- Accurately frames AVC length-prefixed multi-NAL samples without violating codec boundary specifications.
- Maintains strict zero-copy and virtualized memory bounds for multi-gigabyte files.

### Negative / Trade-offs
- Requires adding `stss` and `ctts` to `org.streamview.mp4` (bumping to v0.1.4).
- Requires composite indexing logic combining multiple sample tables.
