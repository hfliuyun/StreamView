# ADR-0104: Generic Structural Payload Dispatch, Transformed Views, and Session-Owned Context Management

- **Status**: Proposed
- **Date**: 2026-08-19
- **Authors**: StreamView Contributors

---

## Context

In Task P5h ([ADR-0102](0102-mp4-sample-descriptions-and-codec-configurations.md)), `org.streamview.mp4` v0.1.3 introduced sample descriptions (`stsd`), sample entries (`avc1`, `mp4a`), and codec configuration boxes (`avcC`, `esds`). Within these structures, individual elementary stream metadata payloads are exposed as `@lazy` byte regions carrying `@target_format`:
- `avcC` repeated sequence parameter set (`sequenceParameterSetNALUnit`) and picture parameter set (`pictureParameterSetNALUnit`) payloads are annotated with `@target_format("video.h264.nal")`;
- `esds` `DecSpecificInfo` (`asc_bytes1..4`) payloads are annotated with `@target_format("audio.aac.asc")`.

In Tasks P5i-1 and P5i-2 ([ADR-0103](0103-cross-layer-structured-entry-execution-and-navigation.md)), `StructuralEntryRunner` and the reusable `BoundedSourceView` established format-neutral execution of isolated single structures (e.g. `AudioSpecificConfig`). However, Task P5i-3 expressibility probing on standalone H.264 NAL parsing identified five fundamental capability gates that prevent standalone NAL execution under DSL 0.1:

### Problem Analysis and Probe Evidence

1. **Lack of Structural Sub-structure / Switch Dispatch**:
   - `tests/probes/p5i3/probe_q1_structural_switch.svfmt` attempted to declare sub-structures inside a `switch` arm within a `struct` definition.
   - Compilation failed at `src/rules/dsl.cpp:1000-1004` with:
     ```text
     error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type
     ```
   - *Fact*: this tested structural-switch form is rejected. DSL 0.1 still has other struct-body control-flow and container forms; this probe does not prove that every nested representation is impossible.

2. **Payload Dispatch Statically Bound to Scan Sequences**:
   - `tests/probes/p5i3/probe_q2_unbound_payload.svfmt` and `tests/probes/p5i3/probe_q2_sequence_entry_mismatch.svfmt` attempted to declare `payload<rbsp>` on non-sequence targets or with mismatched entry declarations.
   - Compilation failed at `src/rules/dsl.cpp:3771-3775` and `src/rules/dsl.cpp:3778-3782` with:
     ```text
     error: A payload dispatch must name a declared sequence
     error: A payload dispatch requires an entry naming its sequence
     ```
   - *Fact*: `payload<view>` is statically restricted to progressive `scan` sequences.

3. **Single Entry Declaration Limitation**:
   - `tests/probes/p5i3/probe_q5_multiple_entries.svfmt` attempted to declare multiple `entry` statements in a single `.svfmt` file.
   - Compilation failed at `src/rules/dsl.cpp:1765-1772` with:
     ```text
     error: A DSL program may contain only one entry
     ```
   - *Fact*: a `.svfmt` file can declare only one top-level `entry`. The manifest currently also rejects duplicate source paths (`src/rules/rule_package.cpp:384-425`), but that is a package-schema policy, not proof that an alternative source layout must duplicate the entire Annex-B file. The target-selection decision below must change that policy or choose another reuse strategy.

4. **Coupled Transformation and Lack of Structural EBSP Unescaping**:
   - *Source Audit Fact*: `H264EbspRbspMapper` is instantiated inside `src/rules/h264_annex_b_analyzer.cpp:666-693`. No generic transformed view layer exists for structural execution. `StructuralEntryRunner::execute` validates the entry at `src/rules/structural_entry_runner.cpp:28-40` and constructs `BitReader(baseSource, sourceMapping)` at `:87-103`, so it currently reads the supplied physical mapping without unescaping support.

5. **Stateless Structural Execution and Missing Context Resolution**:
   - *Source Audit Fact*: `StructuralEntryRunner::execute` (`src/rules/structural_entry_runner.cpp:78-95`) invokes `DslExecutor::decodeStruct` with an empty `DslContextValueResolver()`.
   - When a structure imports context (e.g. `PictureParameterSetRbsp` at `src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt:393-403`), the current structural runner supplies no resolver (`src/rules/structural_entry_runner.cpp:87-103`), and the VM returns `InvalidDefinition` with `Imported context value resolver is unavailable` (`src/rules/dsl_vm.cpp:718-743`). A missing producer in a real session is a separate `DependencyUnavailable` result.
   - `RuleExecutionSession` (`src/rules/rule_execution_session.h:71-100`) owns context publication for the streaming analyzers; it is not yet a reusable compound structural executor and currently binds one source pointer and one tree identity (`src/rules/rule_execution_session.cpp:48-61`).

---

## Decision

To resolve these architectural limitations without introducing H.264-specific orchestration or unconstrained general recursive call stacks, StreamView adopts a bounded structural payload dispatch architecture. “Format-neutral” applies to the compound dispatcher and its contracts; a transform provider may be registered by a format capability and must not leak format names into the generic runner.

This ADR is a design contract only. P5i-3b must implement it in separately reviewable capability commits before any official H.264 package change. P5i-4 remains blocked until those commits are reviewed.

### 1. Structural Payload Dispatch DSL Syntax, AST, and Typed IR

1. **DSL Syntax Extension**:
   Generalize `payload<view_kind>` so it can target either a declared `sequence` (for progressive scanning) or a declared `struct` (for structural entrypoints):
   ```dsl
   payload<view_kind> TargetName switch (controller_field) {
       case V1: PayloadStruct1;
       case V2: PayloadStruct2;
       case V3: empty;
   }
   ```
   When `TargetName` is a `struct`:
   - `controller_field` must be an unsigned scalar `bits<N>` field declared unconditionally at the top level of `TargetName`.
   - Each `case` maps a constant integer value to a declared payload structure name or `empty`.
   - A structural entry point executes a compound structure consisting of `TargetName` followed by at most one dispatched payload.
   - The header end must be byte-aligned before a byte-oriented transform is selected; otherwise compilation or execution fails with `InvalidDefinition` before any transform read.

2. **AST & IR Model**:
   - `DslPayloadTargetKind` enum: `Sequence`, `Structure`.
   - `DslPayloadDispatch`: `targetKind`, `targetName`, `viewKind`, `controllerFieldName`, `cases`.
   - `DslTypedPayloadDispatch`: `targetKind`, `targetIndex` (struct index or scan index), `controllerFieldIndex`, `viewKind`, `cases` (`std::vector<DslTypedPayloadCase>`).

3. **Compiler Validation**:
   - `TargetName` must match a declared struct in `program.structs` (or a scan in `program.scans`).
   - `controllerFieldName` must be an unsigned bits field in `TargetName`.
   - All case targets must be valid declared structs or `empty`.
   - Duplicate cases are rejected as `DuplicateName`; missing targets, non-scalar controllers, and ambiguous target names are typed-definition failures.
   - `viewKind` is an opaque DSL identifier. Parsing and compilation preserve it without consulting or hard-coding the provider set; the runtime registry resolves it, and an unregistered provider fails closed with `InvalidDefinition` before payload decoding.

---

### 2. Unified Tree Hierarchy and Coordinate Attribution

When executing a structural entrypoint with payload dispatch:

1. **Compound transaction and execution modes**:
   - The analysis tree keeps its existing root container. The header structure is an `Indexing` child while both phases run; this is required because `AnalysisTree::appendChild` only accepts an `Indexing` parent (`src/core/analysis_model.cpp:118-135`).
   - The compound executor owns one cumulative instruction/node budget and one cancellation token. It stages context publication and commits it only after header and payload success.
   - A tree snapshot may remove newly appended nodes, but context side effects require a separate staged directory or transaction; `AnalysisTree::restore` alone is insufficient.
   - **Execution Modes**: The runner supports two mutually exclusive payload modes:
     - *Explicit payload mode*: Caller explicitly passes `payloadStructureIndex`, `payloadMapping`, and `transformProviderId` (backward compatible).
     - *Auto typed-dispatch mode*: Caller sets `autoDispatchPayload = true` and supplies `payloadMapping`. The runner dynamically resolves the payload structure and transform provider from `program.payloadDispatch` and the decoded header controller field value.
     - Specifying both explicit `payloadStructureIndex` and `autoDispatchPayload = true` fails closed with `InvalidDefinition`.
     - Requesting auto-dispatch when `program.payloadDispatch` is absent, when the structure resolved from `dispatch.targetKind` and `dispatch.targetIndex` does not match `headerStructureIndex`, or when `payloadMapping` is missing fails closed with `InvalidDefinition`.
     - A sequence target resolves to its element structure. A structure target resolves directly to itself and may be executed either as the selected structural entry or as the element structure of the source file's default sequence entry; this permits manifest target selection without duplicating the shared rule source.

2. **Header phase**:
   - Header fields are decoded into the header child and their `FieldLocation` spans map directly to the input `SourceMapping`.
   - The header bit length is recorded from actual VM consumption. A non-byte-aligned boundary is rejected before any byte-oriented transform.

3. **Dispatch and transformation phase**:
   - In auto-dispatch mode, the runner reads the scalar controller value at `dispatch.controllerFieldIndex` from the decoded header field values. The runner revalidates that this index still names the declared, unconditional unsigned/enum scalar controller in the resolved header structure before trusting public or deserialized typed IR. If the controller field is missing, unpopulated, malformed, or points at another field, execution marks the header `Invalid`, emits `InvalidSyntax`, rolls back exactly once, and fails closed with `InvalidDefinition`.
   - Case resolution:
     - **Structure case**: Maps to a declared payload structure index. The runner queries `PayloadTransformRegistry` using the declared `dispatch.viewKind` stored in typed IR, slices the remaining logical range, transforms the payload input mapping, and decodes the selected payload structure. Execution result exposes `selectedPayloadStructureIndex` and `selectedPayloadCaseValue`.
     - **Empty case**: Maps to no structure (`empty`). No payload structure is decoded; the runner proceeds directly to commit the header-only transaction (`selectedPayloadStructureIndex = std::nullopt`, `selectedPayloadCaseValue` is set).
     - **Unmatched case**: If the controller value does not match any declared case, execution terminates with `Unsupported` status and an `UnsupportedSyntax` diagnostic; the indexing header is marked unsupported and staged context is rolled back.
   - `PayloadTransformProvider` returns a forwarded `SourceMapping`, a separate list of excluded physical spans, inspected-byte accounting, diagnostics, and a terminal status. Excluded bytes are not inserted into `SourceMapping`, because `FieldLocation` requires forwarded span length to equal logical length (`src/core/coordinates.cpp:45-61`).
   - Transform provider lookup is strictly format-neutral: generic runners resolve providers by opaque string identifier (`dispatch.viewKind`) through the registry; generic code must not contain hardcoded format names (e.g. H.264, NAL, SPS, PPS, or RBSP). Unknown providers return `InvalidDefinition`.

4. **Payload phase and finalization**:
   - When a payload structure is selected (explicitly or via matched structure case), it is decoded under the still-indexing header with the remaining compound instruction and node budget.
   - Staged context definitions and imports for the payload phase dynamically use the actually selected payload structure.
   - Header failure commits the partial header and no payload. Payload failure commits the partial tree and diagnostics but rolls back staged context.
   - Only after both header and payload (or header-only in empty case) succeed and commit hooks pass does the compound executor transition nodes to `Materialized`. The final header state is `Invalid`, `WaitingDependency`, `Cancelled`, `Unsupported`, or `Materialized` according to the terminal result; a failed result must not be described as a fully materialized header.

---

### 3. Transformed Source Views and Boundary Error Handling

1. **Transformation Provider**:
   - Generalize the mapping contract, not the codec policy, into `PayloadTransformProvider` / `PayloadTransformRegistry`. The generic runner sees an opaque `view_kind`, a provider result, and terminal statuses; it does not contain NAL, H.264, or H.265 names.
   - `none` is the identity provider. `rbsp` is a capability-registered provider wrapping the existing H.264 mapper policy; its prohibited-sequence diagnostics remain outside the generic dispatcher. Unknown providers return a typed `Unsupported`/`InvalidDefinition` result rather than silently treating bytes as identity.
   - A provider accepts a possibly disjoint logical `SourceMapping`; it must not assume the current mapper's single contiguous `SourceSpan` input.

2. **Span and Coordinate Invariants**:
   - Input `SourceMapping` and the header-to-payload boundary must be byte-aligned and non-empty.
   - The transformed `SourceMapping` contains only forwarded spans. Excluded spans (`0x03` bytes for the registered RBSP provider) are returned separately as `{sourceSpan, outputBitOffset}` records so raw selection can highlight them without violating `FieldLocation` length invariants.
   - Arithmetic on bit offsets is strictly checked against `std::numeric_limits<quint64>::max()` to prevent coordinate overflow.

3. **Error Propagation**:
   - `EndOfSource` / truncation during payload decoding triggers `DslExecutionStatus::TruncatedSource`.
   - Fault-injected `SourceReadStatus::Error` triggers `DslExecutionStatus::SourceError`.
   - Cancellation token checks occur before header execution, before transformation mapping, and during VM instruction loops.
   - Resource limits are split into a shared compound instruction/node budget and a provider inspection budget; both are cumulative and checked for overflow. Cancellation and source errors are terminal for the current compound operation.
   - Provider inspection budget is charged only after a structure case is selected. An `empty` or unmatched case performs no transform inspection and remains executable when the session inspection budget has reached zero.
   - A throwing transform/context resolver factory is treated as an invalid runtime definition: the runner catches the exception, marks the header `Invalid`, rolls back exactly once, and returns `InvalidDefinition` rather than allowing an exception to escape its API.
   - Structural cancellation must define whether a committed prefix is resumable; it must not inherit Annex-B's “committed NAL is never retried” rule without an explicit contract.

---

### 4. Session-Owned Context Lifecycle Management

1. **Session Ownership Principle**:
   - Context directories cannot be passed as unmanaged pointers to stateless runners.
   - P5i-3b must add a compound structural execution API around the existing `RuleExecutionSession`; simply calling its current one-structure `run` twice is insufficient because each call has its own limits and the session currently binds one source pointer and one tree identity (`src/rules/rule_execution_session.cpp:48-61`).
   - The context owner is scoped to one source identity, package/program, and context scope. It may serve multiple presentation trees only after the context definition records stop relying on an ambiguous tree-local `AnalysisNodeId`, or all related nodes are kept in one tree. Navigation must not silently share context across different sources.

2. **Context Lifecycle Across Structural Executions**:
   - A producer definition is staged until exact consumption, imports, dependency generations, and registration all succeed; payload failure must not leak a producer into the directory.
   - A consumer resolves by source position and exact dependency generation, not merely by call order. Missing and stale producers are `DependencyUnavailable`; an absent resolver is the separate `InvalidDefinition` already observed at `src/rules/dsl_vm.cpp:718-743`.
   - Context reset requires an explicit session clear/replace API. The current fixed `contextScopeId` has no reset operation, so P5i-3b must add one or document replacement as the only reset mechanism.
   - For auto dispatch, payload context envelope validation and resolver construction occur only after the controller selects a structure case and before that payload VM starts. Context requirements in unselected cases must not reject an `empty` or context-free selected case.

---

### 5. Rule Package Entrypoint Target Resolution

We evaluate four potential reuse options for multi-entrypoint packages:

1. **Option 1: Multiple `entry` statements in single `.svfmt`**:
   - Causes grammar ambiguity over which entry is active without target naming.
2. **Option 2: Manifest Target Selection (`rule.toml` target selection)**:
   - This is the selected direction, but it is a manifest schema change, not a documentation-only field addition. Manifest version 2 adds optional `target = "StructName"`/`"SequenceName"` to an entrypoint; version 1 remains readable with the target omitted.
   - The compiler receives one shared `compileForTarget(program, optional target)` operation. Omitted target uses the file's default `entry`; specified target resolves exactly one struct or scan and binds the typed entry. Unknown targets, struct/scan ambiguity, malformed identifiers, and target/source mismatches are rejected before execution.
   - v2 permits multiple entrypoints to share a source path only when their `(source, target-or-default)` pair is unique. `RulePackageEntryPoint` carries the optional target, and all analyzers and structural navigation use the same target-aware compiler path.
3. **Option 3: Separate source files (`path = "src/h264_nal.svfmt"`)**:
   - Avoids manifest schema work but may duplicate only the required structure subset; it is not inherently a full-file copy. It remains a fallback if target-aware compilation cannot preserve package compatibility.
4. **Option 4: Cross-file struct module imports**:
   - Excessive compiler complexity for v0.1.

**Decision**: Adopt **Option 2 (Manifest Target Selection)** subject to the v2 schema and compiler contract above. Changing the official manifest changes its content hash; the H.264 package must therefore receive a SemVer/content-version update in the later package slice. Catalog lookup remains selection-only; target binding belongs to the shared compiler API and must be tested separately.

---

### 6. Progressive Annex-B Runner Convergence

- The progressive `H264AnnexBAnalyzer` and structural execution will share the transform-provider contract and policy implementation, but not necessarily the same compound-session lifetime.
- The existing Annex-B mapper's incremental prefix, issue, cancellation, and retry semantics must be preserved by an adapter; a mechanical rename is not sufficient.
- Existing Annex-B behavior is a later regression gate. This ADR does not claim any analyzer count has been rerun.

---

### 7. Safety, Complexity Bounding, and Format Neutrality

- **Strictly Bounded Execution**: Structural payload dispatch is strictly single-level (one header + at most one payload structure). General recursive call stacks are forbidden.
- **Format-Neutral Orchestration**: The generic dispatcher contains no format-specific field names or FourCCs. Existing context kinds and registered transform providers may carry format policy; that policy must remain outside the generic dispatcher and be explicitly registered.
---

### 8. Official H.264 Standalone NAL Package Slice (P5i-3c)

1. **Package Activation Contract**:
   - `src/rules/official/org.streamview.h264/rule.toml` is upgraded to `manifest-version = 2` and package version `0.1.40`.
   - The existing entrypoint `id = "annex-b"` for format `video.h264.annex-b` remains unchanged with no explicit target, compiling to the file's default `entry nal_units` sequence.
   - A new entrypoint `id = "nal"` for format `video.h264.nal` is declared on the same source `src/h264_annex_b.svfmt` with `target = "NalUnitHeader"`.
   - Reuses the existing 1200+ lines of DSL rules without duplicating syntax files.

2. **Execution Semantics**:
   - When compiled with target `NalUnitHeader`, `RulePackageCatalog::resolveByFormat(QStringLiteral("video.h264.nal"))` resolves the entrypoint and `DslCompiler::compileForTarget` binds `NalUnitHeader` as the structural entry with its associated `payload<rbsp>` dispatch.
   - The standalone NAL execution runs through `CompoundStructuralRunner` and `RuleExecutionSession::runCompound` with `autoDispatchPayload = true`:
     - Header (`NalUnitHeader`) is parsed and decoded.
     - Controller field `nal_unit_type` dynamically determines the RBSP payload structure (e.g. `SequenceParameterSetRbsp` for type 7, `PictureParameterSetRbsp` for type 8, `AccessUnitDelimiterRbsp` for type 9, empty for types 10/11, or unhandled returning `Unsupported`).
     - RBSP unescape transformation is performed by the registered `H264RbspPayloadTransformProvider` (`"rbsp"`). Forwarded mapping maps back to physical coordinates, while `0x03` emulation prevention bytes are retained as separate excluded spans without corrupting physical byte offsets.
     - Context definition from SPS (`h264-sps`) is published in the `RuleExecutionSession` upon successful atomic completion, allowing subsequent PPS executions in the same session to resolve `context_value(sps_id, h264_sps, ...)` imports.
     - Dependency unavailability (e.g. PPS executed without prior SPS) returns `RuleExecutionStatus::DependencyUnavailable` without publishing broken context definitions.
     - Truncated source or syntax/conformance errors roll back atomically without context pollution.

3. **Single-Mapping Compound Execution Contract (P5i-4a)**:
   - When caller supplies a single contiguous `SourceMapping` covering the entire sub-format payload (`headerMapping == payloadMapping` with `payloadLogicalStart == 0` and `autoDispatchPayload = true`), header consumption is bounded by the declared header fields rather than the full mapping length.
   - If a payload structure case is selected, the payload input logical start is automatically established at `headerBitsConsumed`, with input bit length `mapping.logicalBitLength() - headerBitsConsumed`.
   - If an empty case is selected, exact consumption requires `headerBitsConsumed == mapping.logicalBitLength()`.
   - This ensures callers (such as `AnalysisSession::enterChildFormat`) execute sub-formats completely format-neutrally without prior knowledge of header sizes or manual span splitting.

---

## P5i-3b Acceptance Matrix (Planned; Not Executed in P5i-3a)

| Test Identifier | Category | Input Fixture / Condition | Execution Path | Expected Status | Key Assertions |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `test_parser_structural_payload_dispatch` | DSL Parser / IR | Valid and malformed `payload<rbsp> Struct switch` declarations | `DslCompiler::compile` | Success for valid; specific diagnostics for invalid | AST and Typed IR correctly encode `DslPayloadTargetKind::Structure`, controller index, and cases. |
| `test_structural_payload_header_and_payload_success` | Core Execution | Synthetic NAL fixture with header + SPS-like payload | Compound structural execution | `Materialized` | Header remains `Indexing` until payload append; final tree and coordinates are exact. |
| `test_structural_payload_ebsp_excluded_spans` | Transformation | Synthetic NAL payload containing `0x00000301`, including a disjoint input mapping | Registered transform provider | `Materialized` | Forwarded mapping and separate excluded-span records are correct; malformed sequences fail closed. |
| `test_structural_payload_empty_and_default_cases` | Edge Cases | Controller matching `empty` case and unhandled case | Structural payload runner | `Materialized` / `Unsupported` | Empty case produces materialized header with no payload child; unhandled case emits unsupported diagnostic. |
| `test_structural_payload_truncation_and_error` | Fault Injection | Truncated payload bytes and I/O error injected source | Compound structural execution | `TruncatedSource` / `SourceError` | Partial tree retains header; staged context is rolled back; payload node carries the matching diagnostic. |
| `test_structural_payload_cancel_and_budget` | Limits / Cancellation | Pre-cancel, mid-transform cancel, and low cumulative header+payload limits | Compound structural execution | `Cancelled` / `ResourceLimit` | Shared instruction/node budget and transform inspection budget are cumulative and non-overflowing. |
| `test_session_context_producer_consumer_chain` | Context Lifecycle | Sequential execution of SPS producer followed by PPS consumer in same `RuleExecutionSession` | `RuleExecutionSession::run` on SPS then PPS | `Materialized` on both | PPS imports SPS context values (`profile_idc`, `chroma_format_idc`) successfully. |
| `test_session_context_missing_dependency` | Context Lifecycle | Execution of PPS consumer without prior SPS producer | `RuleExecutionSession::run` on PPS alone | `DependencyUnavailable` | Returns `DependencyUnavailable` status with diagnostic identifying missing context ID. |
| `test_manifest_target_resolution` | Package / Manifest | Manifest v2 with shared source and distinct `target` values | Catalog plus `compileForTarget` | `Found` for each target | Unknown target, duplicate `(source,target)`, omitted default target, and struct/scan ambiguity are rejected. |
| `test_context_transaction_and_tree_identity` | Context Lifecycle | Producer/payload failure, stale generation, and a second presentation tree | Compound context session | `DependencyUnavailable` / rollback | No failed compound publishes a definition; source identity is enforced without ambiguous tree-local IDs. |
| `test_h264_annex_b_zero_regression` | Regression | Full `tests/rules/h264_annex_b_analyzer_test.cpp` suite | `H264AnnexBAnalyzer` on Annex-B bitstream | `Success` (all tests pass) | Full 174/174 Qt test cases pass with identical AST output. |
| `test_aac_mp4_zero_regression` | Regression | All AAC and MP4 analyzer test suites | `AacAdtsAnalyzer`, `Mp4IsobmffAnalyzer` | `Success` (all tests pass) | Zero regressions across all official formats. |

---

## Consequences

### Positive
- **Format-Neutral Modularity**: Enables decoding of MP4 codec configuration payloads (`avcC` SPS/PPS) through clean declarative DSL rules without C++ format coupling.
- **Shared Source Contract**: Manifest target resolution can allow `video.h264.annex-b` and `video.h264.nal` to share one source file, subject to the v2 parser and target-aware compiler implementation.
- **Deterministic Context Propagation**: A compound context owner can provide audited SPS->PPS resolution without passing an unmanaged directory into the dispatcher.
- **Explicit Transformation Boundary**: Forwarded spans, excluded spans, provider diagnostics, and cancellation/retry semantics are defined separately from tree presentation.

### Negative / Trade-offs
- Requires extending DSL parser, IR, and compiler to support structural `payload` declarations.
- Requires a v2 manifest reader, target-aware compilation, a compound tree/context transaction, and a registered transform provider before official H.264 consumption can begin.
