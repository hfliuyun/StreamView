# ADR-0103: Cross-Layer Structured Entry Execution, Coordinate Mapping, and Navigation Stack

- **Status**: Proposed
- **Date**: 2026-08-18
- **Authors**: StreamView Contributors

---

## Context

In Task P5h ([ADR-0102](0102-mp4-sample-descriptions-and-codec-configurations.md)), `org.streamview.mp4` v0.1.3 introduced sample descriptions (`stsd`), sample entries (`avc1`, `mp4a`), and codec configuration boxes (`avcC`, `esds`). Within these structures, individual elementary stream metadata payloads are exposed as `@lazy` byte regions carrying the `@target_format` annotation:
- `avcC` repeated sequence parameter set (`sequenceParameterSetNALUnit`) and picture parameter set (`pictureParameterSetNALUnit`) payloads are annotated with `@target_format("video.h264.nal")`;
- `esds` `DecSpecificInfo` (`asc_bytes1..4`) payloads are annotated with `@target_format("audio.aac.asc")`.

Task P5h strictly produced AST field metadata without invoking cross-layer decoders. This proposed ADR defines the contract that Tasks P5i-2 through P5i-4 will implement; none of the proposed runner, reusable bounded source, or navigation-stack APIs exists at the P5i-1 baseline.

### Problem Analysis

1. **Structural vs. Streaming Execution Incompatibility**:
   - `org.streamview.aac` defines an entry point for `audio.aac.asc` (`entry AudioSpecificConfig;` at `src/rules/official/org.streamview.aac/src/aac_asc.svfmt:200` and `src/rules/official/org.streamview.aac/rule.toml:22-27`), but `AacAdtsAnalyzer::create` accepts only an ADTS sequence entry (`src/rules/aac_adts_analyzer.cpp:155-215`). Passing raw ASC bytes to `AacAdtsAnalyzer` is therefore invalid.
   - `org.streamview.h264` only defines `video.h264.annex-b` with a start-code scanner sequence (`src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt:1210-1225` and `src/rules/official/org.streamview.h264/rule.toml:14-21`). `H264AnnexBAnalyzer::create` rejects non-sequence entries (`src/rules/h264_annex_b_analyzer.cpp:315-329`), whereas MP4 `avcC` stores raw NAL units without Annex B start codes.
   - The H.264 rule has no `NalUnit` structure or structural entry. Its payload dispatch is attached to the top-level `nal_units` scan. P5i-2 therefore cannot claim complete SPS/PPS decoding; P5i-3 must first prove and implement a standalone one-NAL rule shape, or stop and request a separate language-capability slice.
   - Passing structural data blocks to bitstream scanner analyzers violates the format model and causes scan failures or hangs.

2. **Format Resolution Contract**:
   - `@target_format("format")` stores only a semantic format string.
   - The runtime must look up installed packages using `RulePackageCatalog::resolveByFormat` (`src/rules/rule_catalog.cpp:92-150`) and gracefully report every status that this by-format path can return: `MissingContent`, `VersionConflict`, `IncompatibleLanguage`, and `IncompatibleEngine`.

3. **Coordinate Projection and Source Integrity**:
   - When decoding sub-structures, child AST field locations must project directly back to the physical source spans of the root file ([ADR-0011](0011-dual-coordinates-and-source-mappings.md), [ADR-0024](0024-read-source-mapped-logical-views-without-copying.md)).
   - Copying bytes into a memory buffer and generating detached local `[0, N)` coordinates destroys source mapping and prevents raw data selection highlighting.

4. **Session and Interactive Navigation Stack**:
   - Navigating into a child format must push a navigation frame onto the session stack, switch the active analysis tree and inspector, and maintain two-way synchronization with the raw data view.
   - Returning from a child format must cleanly pop the frame and restore the exact parent selection.
   - Failure to decode a child format must never corrupt or discard the parent analysis tree or UI state.

---

## Decision

### 1. Proposed Structural Entry Runner Contract (`StructuralEntryRunner`)

P5i-2 will introduce a format-neutral structural entry runner in `src/rules/`. The API below is proposed and does not describe a symbol present at the P5i-1 baseline. It deliberately reuses the existing `DslExecutionLimits`, `core::CancellationToken`, `DslTypedProgram`, and `DslExecutionResult` types from `dsl_vm.h` and `dsl_ir.h`:

```cpp
namespace streamview::rules {

struct StructuralExecutionOptions final {
    DslExecutionLimits limits;
    std::optional<core::CancellationToken> cancellation;
};

struct StructuralExecutionResult final {
    DslExecutionResult execution;
    std::shared_ptr<core::AnalysisTree> tree;

    [[nodiscard]] bool succeeded() const noexcept {
        return execution.materialized() && tree != nullptr;
    }
};

class StructuralEntryRunner final {
public:
    [[nodiscard]] static StructuralExecutionResult execute(
        const core::RandomAccessSource& baseSource,
        const core::SourceMapping& sourceMapping,
        const DslTypedProgram& program,
        const StructuralExecutionOptions& options = {});
};

} // namespace streamview::rules
```

#### Behavioral Invariants
1. `StructuralEntryRunner` requires `program.entry.kind == DslEntryKind::Structure` and validates `program.entry.targetIndex` before reading. Sequence entries are rejected as `InvalidDefinition`.
2. It validates that `sourceMapping.logicalBitLength()` is non-zero, byte-aligned, and convertible to bytes. The runner uses the existing mapping-aware `core::BitReader(baseSource, sourceMapping)` path, which reads the physical spans without copying and preserves the VM's reader/mapping consistency check.
3. It calls `DslExecutor::decodeStruct` (`src/rules/include/streamview/rules/dsl_executor.h:13-42`) with `program.entry.targetIndex`, logical start zero, a newly owned child tree, and `DslExecutionOptions` built from the supplied limits and cancellation token. `BoundedSourceView` remains the reusable `RandomAccessSource` view for scanner and other byte-oriented consumers; it is not inserted as a second mapping layer in the VM reader.
4. Each explicit navigation action receives its own bounded execution limits and cancellation token. P5i does not recursively auto-expand target formats and does not share a completed parent analyzer's consumed budget with a later user action.
5. It does **not** instantiate or invoke scanner-based analyzers (`AacAdtsAnalyzer`, `H264AnnexBAnalyzer`, `Mp4IsobmffAnalyzer`). Streaming sequence analyzers are strictly reserved for top-level progressive media files.

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
| `Found` | Exactly one matching package and entry point found | Proceed with compilation and structural execution | Frame is pushed only after successful execution |
| `MissingContent` | No installed rule package exports `entrypoint.format == targetFormat` | Error tooltip / banner: `"No installed rule package matches format '<format>'"` | Navigation rejected; parent state preserved |
| `VersionConflict` | Multiple installed packages export the same format | Error tooltip / banner: `"Multiple installed package entry points match format '<format>'"` | Navigation rejected; parent state preserved |
| `IncompatibleLanguage` | Matching package requires an unsupported DSL version | Error tooltip / banner: `"Package '<id>' requires DSL <req>, running DSL is <curr>"` | Navigation rejected; parent state preserved |
| `IncompatibleEngine` | Matching package requires an incompatible engine version | Error tooltip / banner: `"Package '<id>' requires engine <req>, running engine is <curr>"` | Navigation rejected; parent state preserved |

`@target_format` retains its clean contract: it contains only the standard format string (e.g., `"video.h264.nal"`, `"audio.aac.asc"`), and never hardcodes package identifiers or version constraints.

---

### 3. Coordinate Mapping and Source Projection Contract

To establish child AST coordinates mapped directly to root source bytes:

1. **Source Span Extraction**: The parent lazy field node must possess a valid `FieldLocation` with non-empty `sourceSpans()` (`src/core/include/streamview/core/coordinates.h:103-121`).
2. **Alignment & Byte Validation**:
   - If `sourceSpans().empty()`, navigation preflight fails with `InvalidTargetLocation` (`"Target node has no source location"`); this is not a VM `InvalidSyntax` result because execution has not started.
   - If the logical range or any physical span is not byte-aligned, navigation preflight fails with `InvalidTargetLocation` (`"Target node source location is not byte-aligned"`).
   - The total byte length is `byteLength = logicalRange().bitLength() / 8`.
3. **Disjoint & Multi-Span Stitching**:
   - `core::SourceMapping::create(childViewId, targetNode.location()->sourceSpans())` builds a logical view address space `[0, byteLength * 8)`. A `std::nullopt` result rejects navigation before any child tree is activated.
   - `BoundedSourceView` reads across all disjoint spans in mapping order and uses `SourceMapping::locate` for every requested logical range.
   - Logical bit offset zero maps to the first span start; all later child ranges, including ranges crossing span boundaries, use the full `FieldLocation::sourceSpans()` returned by `locate` rather than assuming the first span.
4. **Boundary & Read Error Handling**:
   - Reads past `byteLength` return `SourceReadStatus::EndOfSource` and trigger `DslExecutionStatus::TruncatedSource` on the child tree.
   - Underlying physical I/O faults return `SourceReadStatus::Error` and trigger `DslExecutionStatus::SourceError`.
5. **Reusable View Boundary**: A similar `BoundedSourceView` currently exists only as a private class in `mp4_isobmff_analyzer.cpp:84-142`. P5i-2 must extract or replace it with a reusable rules/core module and add it to the owning CMake target; the ADR does not claim that a public view already exists.
6. **Zero-Copy Guarantee**: Payload bytes are never copied into detached memory buffers. All reads go through the reusable mapped view and `core::BitReader`.

---

### 4. Proposed Session and UI Navigation Stack Contract

`AnalysisSession` has no navigation API at the P5i-1 baseline (`src/app/analysis_session.h:64-177`). P5i-4 will add the semantic navigation stack below. `MainWindow` remains responsible for `AnalysisTreeModel`, `FieldInspector`, `RawDataView`, breadcrumbs, and presentation-state snapshots; the non-`QObject` session must not emit the signals shown in the original draft or directly manipulate widgets.

```cpp
namespace streamview::app {

struct NavigationFrame final {
    core::AnalysisNodeId parentTargetNodeId;
    QString targetFormat;
    std::shared_ptr<const rules::RulePackage> package;
    rules::RulePackageEntryPoint entryPoint;
    core::SourceMapping sourceMapping;
    std::shared_ptr<core::AnalysisTree> tree;
};

} // namespace streamview::app
```

`navigationStack_` contains child frames only. When it is empty, the root analyzer tree is active; otherwise `navigationStack_.back().tree` is active. Each frame's `parentTargetNodeId` belongs to the previously active level, so popping a frame deterministically reveals the parent tree and the node that `MainWindow` must reselect.

#### Navigation Operations

1. **Enter Child Format (`AnalysisSession::enterChildFormat(core::AnalysisNodeId nodeId, const RulePackageCatalog& catalog)`)**:
   - Validates that `nodeId` in the active tree has `metadata().targetFormat`.
   - Resolves target format via `RulePackageCatalog::resolveByFormat`.
   - Builds `SourceMapping` and runs `StructuralEntryRunner::execute`.
   - Resolution, preflight, compile, and execution failures are returned in a structured result without pushing a frame. A failed execution may retain an inactive partial tree for diagnostics, but it never replaces the active tree.
   - **Safety Guarantee**: On failure, `MainWindow` keeps the current model, selection, `FieldInspector`, and raw highlight untouched and presents the returned error.
   - If execution succeeds:
     - Pushes `NavigationFrame` to `navigationStack_`.
     - Returns the child tree and its root structure node to `MainWindow`.
     - `MainWindow` records the parent node/selection snapshot, switches `AnalysisTreeModel`, selects the child root, and updates breadcrumbs.

2. **Return to Parent (`AnalysisSession::returnToParent()`)**:
   - If `navigationStack_.empty()`, operation is a no-op.
   - Pops the top `NavigationFrame`.
   - Returns the parent target node identifier and active parent tree to `MainWindow`.
   - `MainWindow` restores the saved parent selection and derives its raw highlight from the restored node location.

3. **Two-Way Coordinate Interaction**:
   - **Tree to Raw View**: Selecting any field in the child `AnalysisTree` retrieves `node->location()->sourceSpans()` and updates `RawDataView` highlight to the exact physical root file byte offsets.
   - **Raw View to Tree**: Clicking a byte in `RawDataView` inspects the active `AnalysisTree` (child tree if navigated) and selects the most specific leaf node enclosing that byte.

---

### 5. Persistence Boundary

In StreamView v0.1:
- The navigation stack is an ephemeral in-process UI interaction state.
- `SessionDocument` (`src/app/session_document.h:40-100`) preserves only the root-session state in v0.1; P5i-4 must not change its schema.
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
[P5i-2 (Rules/Core)]: Reusable Mapped Source View + StructuralEntryRunner & CTest
      │
      ▼
[P5i-3 (Rules/Pkgs)]: Standalone H.264 NAL Rule Shape/Entrypoint + AAC ASC Validation
      │
      ▼
[P5i-4 (App/UI)]: AnalysisSession NavigationStack, MainWindow Breadcrumbs & Bidirectional Selection
```

- **Task P5i-1**: Markdown-only dual-language ADR-0103 specification.
- **Task P5i-2**: Implement the reusable mapped source view and `StructuralEntryRunner`. Prove the generic path with a local structural rule and the existing `audio.aac.asc` entry. Do not change official package manifests or claim complete H.264 NAL decoding in this capability slice.
- **Task P5i-3** (Probe Outcome & Blocked State): Docs-first probes and source audit show that DSL 0.1 has no complete, format-neutral path for a standalone H.264 NAL entry. The evidence is deliberately limited to the tested forms; it does not claim that every future syntax spelling is impossible:
  1. A structural `switch` arm cannot instantiate a child structure. `tests/probes/p5i3/probe_q1_structural_switch.svfmt` fails with `Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` at `src/rules/dsl.cpp:1000-1004`.
  2. `payload<rbsp>` is bound to a declared scan sequence and requires the entry to name that sequence. `tests/probes/p5i3/probe_q2_unbound_payload.svfmt` and `probe_q2_sequence_entry_mismatch.svfmt` reproduce the two diagnostics from `src/rules/dsl.cpp:3763-3781`. The existing EBSP/RBSP mapper is instantiated only by `H264AnnexBAnalyzer` (`src/rules/h264_annex_b_analyzer.cpp:666-693`); `StructuralEntryRunner` has no transform hook.
  3. A standalone PPS execution cannot resolve its SPS import through the current runner: `StructuralEntryRunner::execute` supplies no context resolver (`src/rules/structural_entry_runner.cpp:87-103`), and the VM returns `InvalidDefinition` with `Imported context value resolver is unavailable` (`src/rules/dsl_vm.cpp:718-743`). This limitation applies to PPS and dependent slices, not to an isolated SPS with no import.
  4. Keeping the runner format-neutral means this gap must be solved by a new generic DSL/runtime capability, not by adding H.264 dispatch to C++. The current implementation itself contains no format-specific dispatch.
  5. A `.svfmt` program accepts only one `entry` (`tests/probes/p5i3/probe_q5_multiple_entries.svfmt`, `src/rules/dsl.cpp:1758-1777`). The manifest already supports multiple entrypoints, but each entry must use a distinct source path (`src/rules/rule_package.cpp:371-424`); the repository has no cross-file structure import. It is therefore inaccurate to claim that a new entry necessarily duplicates the entire Annex-B file.
  Per the Stop and Report Protocol, Task P5i-3 stops without modifying `org.streamview.h264` (retains v0.1.39) or adding `video.h264.nal`. The existing `org.streamview.aac` v0.1.4 `audio.aac.asc` entry is verified through the bundled package, `resolveByFormat`, its manifest source, and `StructuralEntryRunner`; it remains the P5i-4 navigation baseline.
- **Task P5i-3a**: Docs-first design of the smallest generic capability needed for a structural payload entry: header/controller parsing, a mapped byte transformation hook, payload-structure dispatch, and session-owned context resolution. Do not implement the official H.264 package in this capability slice.
- **Task P5i-4**: Implement `NavigationStack` in `AnalysisSession`, connect breadcrumb navigation in `MainWindow`, and test tree/raw view bidirectional coordinate synchronization using the verified `audio.aac.asc` path.

---

## Verification Matrix

| Test Identifier | Category | Input Fixture / Condition | Execution Path | Expected Status | Key Assertions |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `executesOfficialAacAscOnEsdsFixture` | P5i-2 Core Execution | `mp4_p5h_mp4a_esds.mp4` (`asc_bytes1`, 2 bytes `0x12 0x10`) | Direct compile of the existing `AudioSpecificConfig` source and `StructuralEntryRunner` | `DslExecutionStatus::Materialized` | Complete ordered six-field list and exact root spans `[1168,1173)`, `[1173,1177)`, `[1177,1181)`, `[1181,1182)`, `[1182,1183)`, `[1183,1184)`. |
| `executesCatalogResolvedAacAscOnEsdsFixture` | P5i-3 Package Verification | Same ASC fixture | Bundled AAC v0.1.4 -> `resolveByFormat("audio.aac.asc")` -> manifest source -> `StructuralEntryRunner` | `DslExecutionStatus::Materialized` | Package/version, `asc` entry metadata, complete child list, `audio_object_type == 2`, and first-field root span are asserted. |
| `resolvesPackageByFormat` | Catalog Lookup Regression | Synthetic package catalog cases | `RulePackageCatalog::resolveByFormat` | `Found`, `MissingContent`, `IncompatibleLanguage`, `IncompatibleEngine` | Existing rule-package test asserts each status and diagnostics. |
| `rejectsAmbiguousPackagesWhenResolvingByFormat` | Catalog Lookup Regression | Two installed versions with the same format | `RulePackageCatalog::resolveByFormat` | `VersionConflict` | Existing rule-package test rejects ambiguous content without returning a package. |
| `p5i3_h264_standalone_probes` | P5i-3 Expressibility Gate | Four committed `.svfmt` probes under `tests/probes/p5i3/` | `svtool rule check` expected failures | Negative diagnostics | Q1/Q2/Q5 diagnostics are reproducible; H.264 SPS/PPS materialization is intentionally blocked until P5i-3a. |
| `test_bounded_source_view_truncated` | P5i-2 Coordinate/Source | Two-byte ASC exposed through a one-byte mapping | `StructuralEntryRunner` execution | `DslExecutionStatus::TruncatedSource` | `TruncatedSource` diagnostic emitted; inactive partial nodes retain exact source spans. |
| `test_bounded_source_view_io_fault` | P5i-2 Coordinate/Source | Fault-injected `RandomAccessSource` | `StructuralEntryRunner` execution | `DslExecutionStatus::SourceError` | `SourceError` returned; any partial tree remains inactive and the parent is untouched. |
| `test_navigation_enter_and_return_cycle` | P5i-4 Session/UI | `AnalysisSession` on `mp4_p5h_mp4a_esds.mp4` | `enterChildFormat(ascNodeId)` then `returnToParent()` | Success | Stack depth transitions 0 -> 1 -> 0; active tree switches to ASC AST then back to MP4 AST; parent selected node restored. |
| `test_navigation_enter_failure_preserves_parent` | P5i-4 Session/UI | Target node with invalid/missing format `"invalid.format"` | `enterChildFormat(nodeId)` | `MissingContent` / Failure | Structured failure returned; stack depth remains 0; parent tree and selection unchanged. |
| `test_bidirectional_coordinate_selection` | P5i-4 Coordinates/UI | Child ASC tree node selected | Inspect `FieldLocation` source spans | Success | ASC payload maps to root bytes `[146, 148)` in the committed fixture; selecting absolute source bit 1173 selects `AudioSpecificConfig.sampling_frequency_index`. |
| `test_nested_navigation_independent_limits` | P5i-4 Budget/Cancellation | Explicit nested navigation with low child limits | A second user-triggered child execution | `ResourceLimit` | Each navigation action enforces its own limits; child cancellation/resource failure leaves the completed parent frame usable. |

---

## Consequences

### Positive
- **Format-Neutral Modularity**: Elementary stream formats (H.264 NAL, AAC ASC) are decoded by clean declarative rules without polluting the MP4 analyzer core with codec-specific C++ logic.
- **Accurate Source Attribution**: Every decoded syntax field in sub-trees directly highlights the true physical bytes in the original container file.
- **Robust UI Experience**: Clear breadcrumb navigation, full failure isolation, and seamless bidirectional coordinate mapping.

### Negative / Trade-offs
- Requires introducing `StructuralEntryRunner` and, where the existing rule shape is insufficient, adding a structural entry point to the relevant official package.
