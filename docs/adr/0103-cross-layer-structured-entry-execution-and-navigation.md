# ADR-0103: Cross-Layer Structured Entry Execution, Coordinate Mapping, and Navigation Stack

- **Status**: Proposed
- **Date**: 2026-08-18
- **Authors**: StreamView Contributors

---

## Context

In Task P5h ([ADR-0102](0102-mp4-sample-descriptions-and-codec-configurations.md)), `org.streamview.mp4` v0.1.3 introduced sample descriptions (`stsd`), sample entries (`avc1`, `mp4a`), and codec configuration boxes (`avcC`, `esds`). Within these structures, individual elementary stream metadata payloads are exposed as `@lazy` byte regions carrying the `@target_format` annotation:
- `avcC` repeated sequence parameter set (`sequenceParameterSetNALUnit`) and picture parameter set (`pictureParameterSetNALUnit`) payloads are annotated with `@target_format("video.h264.nal")`;
- `esds` `DecSpecificInfo` (`asc_bytes1..4`) payloads are annotated with `@target_format("audio.aac.asc")`.

Task P5h strictly produced AST field metadata without invoking cross-layer decoders. Task P5i implements the runtime execution, coordinate projection, and interactive UI navigation for these target formats.

### Problem Analysis

1. **Structural vs. Streaming Execution Incompatibility**:
   - `org.streamview.aac` defines an entry point for `audio.aac.asc` (`entry AudioSpecificConfig;` in [`src/rules/official/org.streamview.aac/src/aac_asc.svfmt:200`](file:///Users/yun/code/streamview/src/rules/official/org.streamview.aac/src/aac_asc.svfmt#L200) and [`src/rules/official/org.streamview.aac/rule.toml:23-28`](file:///Users/yun/code/streamview/src/rules/official/org.streamview.aac/rule.toml#L23-L28)), but [`AacAdtsAnalyzer`](file:///Users/yun/code/streamview/src/rules/aac_adts_analyzer.cpp#L157) only executes scanning sequences over continuous ADTS syncwords. Passing raw ASC bytes to `AacAdtsAnalyzer` fails immediately.
   - `org.streamview.h264` only defines `video.h264.annex-b` with a start-code scanner sequence in [`src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt:1210-1226`](file:///Users/yun/code/streamview/src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt#L1210-L1226) and [`src/rules/official/org.streamview.h264/rule.toml:14-21`](file:///Users/yun/code/streamview/src/rules/official/org.streamview.h264/rule.toml#L14-L21). `H264AnnexBAnalyzer` requires 3/4-byte start codes (`0x000001` / `0x00000001`), whereas MP4 `avcC` stores raw length-delimited NAL units without Annex B start codes.
   - Passing structural data blocks to bitstream scanner analyzers violates the format model and causes scan failures or hangs.

2. **Format Resolution Contract**:
   - `@target_format("format")` stores only a semantic format string.
   - The runtime must look up installed packages using [`RulePackageCatalog::resolveByFormat`](file:///Users/yun/code/streamview/src/rules/rule_catalog.cpp#L92-L150) and gracefully report all resolution failure states (`MissingContent`, `VersionConflict`, `IncompatibleLanguage`, `IncompatibleEngine`).

3. **Coordinate Projection and Source Integrity**:
   - When decoding sub-structures, child AST field locations must project directly back to the physical source spans of the root file ([ADR-0011](0011-dual-coordinates-and-source-mappings.md), [ADR-0024](0024-read-source-mapped-logical-views-without-copying.md)).
   - Copying bytes into a memory buffer and generating detached local `[0, N)` coordinates destroys source mapping and prevents raw data selection highlighting.

4. **Session and Interactive Navigation Stack**:
   - Navigating into a child format must push a navigation frame onto the session stack, switch the active analysis tree and inspector, and maintain two-way synchronization with the raw data view.
   - Returning from a child format must cleanly pop the frame and restore the exact parent selection.
   - Failure to decode a child format must never corrupt or discard the parent analysis tree or UI state.

---

## Decision

### 1. Structural Entry Runner Contract (`StructuralEntryRunner`)

We introduce a format-neutral structural entry runner in `src/rules/` (`streamview::rules::StructuralEntryRunner`):

```cpp
namespace streamview::rules {

struct StructuralExecutionOptions final {
    RunnerLimits limits;
    core::CancellationSource::Token cancellation;
};

struct StructuralExecutionResult final {
    DslExecutionStatus status = DslExecutionStatus::InvalidDefinition;
    std::shared_ptr<core::AnalysisTree> tree;
    std::optional<core::AnalysisNodeId> rootStructNode;
    std::size_t nodesCreated = 0;
    std::size_t instructionsExecuted = 0;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == DslExecutionStatus::Complete && tree != nullptr && rootStructNode.has_value();
    }
};

class StructuralEntryRunner final {
public:
    [[nodiscard]] static StructuralExecutionResult execute(
        const core::RandomAccessSource& baseSource,
        const core::SourceMapping& sourceMapping,
        quint64 byteLength,
        const DslProgram& program,
        quint32 entryStructIndex,
        const StructuralExecutionOptions& options = {});
};

} // namespace streamview::rules
```

#### Behavioral Invariants
1. `StructuralEntryRunner` takes a `baseSource`, `sourceMapping`, and `byteLength`, creating a `BoundedSourceView` over the exact mapped source region.
2. It initializes a `core::BitReader` bound to the `BoundedSourceView` and executes [`DslExecutor::decodeStruct`](file:///Users/yun/code/streamview/src/rules/include/streamview/rules/dsl_executor.h#L58-L75) directly against `entryStructIndex`.
3. It does **not** instantiate or invoke scanner-based analyzers (`AacAdtsAnalyzer`, `H264AnnexBAnalyzer`, `Mp4IsobmffAnalyzer`). Streaming sequence analyzers are strictly reserved for top-level progressive media files.

---

### 2. Target Format Resolution Contract

When a user or session requests navigation into a lazy node annotated with `@target_format`, the engine invokes:

```cpp
RuleCatalogLookupResult result = catalog.resolveByFormat(
    targetFormat, runningLanguageVersion, runningEngineVersion);
```

The lookup outcomes map to specific user-facing actions and diagnostics:

| `RuleCatalogLookupStatus` | Internal Cause | User-Facing Diagnostic / UX Action | Navigation State |
| :--- | :--- | :--- | :--- |
| `Found` | Exactly one matching package and entry point found | Proceed with compilation and structural execution | Pushes new `NavigationFrame` |
| `MissingContent` | No installed rule package exports `entrypoint.format == targetFormat` | Error tooltip / banner: `"No installed rule package matches format '<format>'"` | Navigation rejected; parent state preserved |
| `VersionConflict` | Multiple installed packages export the same format | Error tooltip / banner: `"Multiple installed package entry points match format '<format>'"` | Navigation rejected; parent state preserved |
| `IncompatibleLanguage` | Matching package requires an unsupported DSL version | Error tooltip / banner: `"Package '<id>' requires DSL <req>, running DSL is <curr>"` | Navigation rejected; parent state preserved |
| `IncompatibleEngine` | Matching package requires an incompatible engine version | Error tooltip / banner: `"Package '<id>' requires engine <req>, running engine is <curr>"` | Navigation rejected; parent state preserved |

`@target_format` retains its clean contract: it contains only the standard format string (e.g., `"video.h264.nal"`, `"audio.aac.asc"`), and never hardcodes package identifiers or version constraints.

---

### 3. Coordinate Mapping and Source Projection Contract

To establish child AST coordinates mapped directly to root source bytes:

1. **Source Span Extraction**: The parent lazy field node must possess a valid [`FieldLocation`](file:///Users/yun/code/streamview/src/core/include/streamview/core/coordinates.h#L103-L121) with non-empty `sourceSpans()`.
2. **Alignment & Byte Validation**:
   - If `sourceSpans().empty()`, navigation fails with `InvalidSyntax` (`"Target node has no source location"`).
   - If `logicalRange().bitLength() % 8 != 0`, navigation fails with `InvalidSyntax` (`"Target node source location is not byte-aligned"`).
   - The total byte length is `byteLength = logicalRange().bitLength() / 8`.
3. **Disjoint & Multi-Span Stitching**:
   - `core::SourceMapping::create(childViewId, targetNode.location()->sourceSpans())` builds a logical view address space `[0, byteLength * 8)`.
   - `BoundedSourceView` transparently reads across all disjoint spans in logical order.
   - Logical bit offset 0 in the child AST automatically maps back to `targetNode.location()->sourceSpans().front().start()`.
4. **Boundary & Read Error Handling**:
   - Reads past `byteLength` return `SourceReadStatus::EndOfSource` and trigger `DslExecutionStatus::TruncatedSource` on the child tree.
   - Underlying physical I/O faults return `SourceReadStatus::Error` and trigger `DslExecutionStatus::SourceError`.
5. **Zero-Copy Guarantee**: Payload bytes are never copied into detached memory buffers. All reads go through `BoundedSourceView` and `core::BitReader`.

---

### 4. Session and UI Navigation Stack Contract

The navigation stack is owned by [`AnalysisSession`](file:///Users/yun/code/streamview/src/app/analysis_session.h#L64-L177):

```cpp
namespace streamview::app {

struct NavigationFrame final {
    core::AnalysisNodeId parentTargetNodeId;
    QString targetFormat;
    std::shared_ptr<const rules::RulePackage> package;
    rules::RulePackageEntryPoint entryPoint;
    core::SourceMapping sourceMapping;
    std::shared_ptr<core::AnalysisTree> tree;
    std::optional<core::AnalysisNodeId> lastSelectedNodeId;
    std::optional<core::SourceSelection> lastSourceSelection;
};

} // namespace streamview::app
```

#### Navigation Operations

1. **Enter Child Format (`AnalysisSession::enterChildFormat(core::AnalysisNodeId nodeId)`)**:
   - Validates that `nodeId` in the active tree has `metadata().targetFormat`.
   - Resolves target format via `RulePackageCatalog::resolveByFormat`.
   - Builds `SourceMapping` and runs `StructuralEntryRunner::execute`.
   - If execution produces `ResourceLimit`, `Cancelled`, `SourceError`, `InvalidRule`, or `MissingContent`:
     - Emits `childNavigationFailed(QString errorMessage)`.
     - **Safety Guarantee**: Does not push a stack frame; leaves the active tree, selection, and `FieldInspector` completely untouched.
   - If execution succeeds:
     - Records current selection in the active frame.
     - Pushes `NavigationFrame` to `navigationStack_`.
     - Updates `AnalysisTreeModel` to display the child tree.
     - Sets active selection to the child root structure node.
     - Emits `navigationStackChanged()`.

2. **Return to Parent (`AnalysisSession::returnToParent()`)**:
   - If `navigationStack_.empty()`, operation is a no-op.
   - Pops the top `NavigationFrame`.
   - Restores the parent frame's `AnalysisTree` in `AnalysisTreeModel`.
   - Restores `lastSelectedNodeId` and raw data selection highlight.
   - Emits `navigationStackChanged()`.

3. **Two-Way Coordinate Interaction**:
   - **Tree to Raw View**: Selecting any field in the child `AnalysisTree` retrieves `node->location()->sourceSpans()` and updates `RawDataView` highlight to the exact physical root file byte offsets.
   - **Raw View to Tree**: Clicking a byte in `RawDataView` inspects the active `AnalysisTree` (child tree if navigated) and selects the most specific leaf node enclosing that byte.

---

### 5. Persistence Boundary

In StreamView v0.1:
- The navigation stack is an ephemeral in-process UI interaction state.
- [`SessionDocument`](file:///Users/yun/code/streamview/src/app/session_document.h#L40-L100) preserves the root analysis file, verified fingerprint, root bookmarks, and notes.
- When extending session schema in future versions:
  - Navigation path entries store `parent_node_path` and `target_format`.
  - If a rule package is missing or parent path cannot be found during restore, the session opens at the root tree gracefully without failing document restoration.

---

### 6. Implementation Slice Plan

To prevent monolithic PRs and enforce single-responsibility review:

```
[P5i-1 (Docs)]: ADR-0103 Architecture Specification & Verification Contract
      │
      ▼
[P5i-2 (Rules/Core)]: BoundedSourceView + StructuralEntryRunner Implementation & CTest
      │
      ▼
[P5i-3 (Rules/Pkgs)]: H.264 video.h264.nal Entrypoint & AAC ASC Structural Manifest Update
      │
      ▼
[P5i-4 (App/UI)]: AnalysisSession NavigationStack, MainWindow Breadcrumbs & Bidirectional Selection
```

- **Task P5i-1** (this task): Markdown-only dual-language ADR-0103 specification.
- **Task P5i-2**: Implement `StructuralEntryRunner`, source coordinate projection, and unit tests in `streamview_dsl_executor_tests`.
- **Task P5i-3**: Add `video.h264.nal` structural entrypoint in `org.streamview.h264` v0.1.40 and verify `org.streamview.aac` v0.1.5 structural execution.
- **Task P5i-4**: Implement `NavigationStack` in `AnalysisSession`, connect breadcrumb navigation in `MainWindow`, and test tree/raw view bidirectional coordinate synchronization.

---

## Verification Matrix

| Test Identifier | Category | Input Fixture / Condition | Execution Path | Expected Status | Key Assertions |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `test_structural_runner_aac_asc_success` | Core Execution | `mp4_p5h_mp4a_esds.mp4` (`asc_bytes1`, 2 bytes `0x12 0x10`) | `StructuralEntryRunner` on `AudioSpecificConfig` | `DslExecutionStatus::Complete` | Root node name `AudioSpecificConfig`; `audio_object_type == 2`, `sampling_frequency_index == 4`, `channel_configuration == 2`; source spans match parent byte offsets exactly. |
| `test_structural_runner_h264_sps_nal_success` | Core Execution | `mp4_p5h_avc1_avcC.mp4` (`sequenceParameterSetNALUnit[0]`, 25 bytes) | `StructuralEntryRunner` on `NalUnit` | `DslExecutionStatus::Complete` | `nal_unit_type == 7`, `profile_idc == 100`, `level_idc == 41`; child field source spans match root SPS byte offsets. |
| `test_structural_runner_h264_pps_nal_success` | Core Execution | `mp4_p5h_avc1_avcC.mp4` (`pictureParameterSetNALUnit[0]`, 4 bytes) | `StructuralEntryRunner` on `NalUnit` | `DslExecutionStatus::Complete` | `nal_unit_type == 8`, `pic_parameter_set_id == 0`, `seq_parameter_set_id == 0`; source spans match root PPS byte offsets. |
| `test_resolve_by_format_all_statuses` | Catalog Lookup | Formats: `"audio.aac.asc"`, `"video.h264.nal"`, `"unknown.fmt"`, simulated conflict, incompatible versions | `RulePackageCatalog::resolveByFormat` | `Found`, `MissingContent`, `VersionConflict`, `IncompatibleLanguage`, `IncompatibleEngine` | Correct lookup status returned with non-empty diagnostic message. |
| `test_bounded_source_view_truncated` | Coordinate/Source | 25-byte SPS with `BoundedSourceView` limited to 10 bytes | `StructuralEntryRunner` execution | `DslExecutionStatus::TruncatedSource` | `TruncatedSource` diagnostic emitted; partial nodes preserved with exact source spans. |
| `test_bounded_source_view_io_fault` | Coordinate/Source | Fault-injected `RandomAccessSource` | `StructuralEntryRunner` execution | `DslExecutionStatus::SourceError` | `SourceError` status returned; tree creation cleanly aborted. |
| `test_navigation_enter_and_return_cycle` | Session/UI | `AnalysisSession` on `mp4_p5h_avc1_avcC.mp4` | `enterChildFormat(spsNodeId)` then `returnToParent()` | Complete | Stack depth transitions 0 -> 1 -> 0; active tree switches to SPS AST then back to MP4 AST; parent selected node restored. |
| `test_navigation_enter_failure_preserves_parent` | Session/UI | Target node with invalid/missing format `"invalid.format"` | `enterChildFormat(nodeId)` | `MissingContent` / Failure | `childNavigationFailed` emitted; stack depth remains 0; parent tree and selection unchanged. |
| `test_bidirectional_coordinate_selection` | Coordinates/UI | Child ASC tree node selected | Inspect `FieldLocation` source spans | Complete | Child node source span matches root file byte offsets `[184, 186)`; clicking root byte 185 selects `AudioSpecificConfig.sampling_frequency_index`. |
| `test_multilevel_navigation_budget_sharing` | Budget/Limits | Nested navigation with low node/instruction budget | Recursive child format execution | `ResourceLimit` | Shared budget decremented across layers; limits enforced without heap overflow. |

---

## Consequences

### Positive
- **Format-Neutral Modularity**: Elementary stream formats (H.264 NAL, AAC ASC) are decoded by clean declarative rules without polluting the MP4 analyzer core with codec-specific C++ logic.
- **Accurate Source Attribution**: Every decoded syntax field in sub-trees directly highlights the true physical bytes in the original container file.
- **Robust UI Experience**: Clear breadcrumb navigation, full failure isolation, and seamless bidirectional coordinate mapping.

### Negative / Trade-offs
- Requires introducing `StructuralEntryRunner` and defining structural entry points across official rule packages.
