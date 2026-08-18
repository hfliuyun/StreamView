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
   - *Fact*: DSL 0.1 struct bodies only support primitive fields (`bits`, `ue`, `se`, `ff_coded`), computed fields, lazy regions, `compressed_payload`, assertions, and trailers. Structs cannot instantiate nested sub-structures or invoke structures from `switch` arms.

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
   - *Fact*: A `.svfmt` file can declare only one top-level `entry`. Because DSL 0.1 lacks cross-file module imports, creating a separate `.svfmt` for NAL units would require duplicating all 1200+ lines of H.264 syntax.

4. **Coupled Transformation and Lack of Structural EBSP Unescaping**:
   - *Source Audit Fact*: `H264EbspRbspMapper` is instantiated exclusively inside `src/rules/h264_annex_b_analyzer.cpp:688`. No generic transformed view layer exists for structural execution. `StructuralEntryRunner::execute` (`src/rules/structural_entry_runner.cpp:30-40`) reads physical bytes directly via `BitReader(baseSource, sourceMapping)` without unescaping support.

5. **Stateless Structural Execution and Missing Context Resolution**:
   - *Source Audit Fact*: `StructuralEntryRunner::execute` (`src/rules/structural_entry_runner.cpp:78-95`) invokes `DslExecutor::decodeStruct` with an empty `DslContextValueResolver()`.
   - When a structure imports context (e.g. `import SequenceParameterSet context ...` in `PictureParameterSetRbsp` at `src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt:393-403`), `DslVirtualMachine` (`src/rules/dsl_vm.cpp:718-743`) returns `DslExecutionStatus::InvalidDefinition` / `DependencyUnavailable` because no context directory is injected.
   - `RuleExecutionSession` (`src/rules/rule_execution_session.h:71-100`) contains the complete context publishing and lookup engine, but is currently private to streaming analyzers.

---

## Decision

To resolve these architectural limitations without introducing H.264-specific C++ logic or unconstrained general recursive call stacks, StreamView adopts a generic, format-neutral structural payload dispatch architecture.

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
   - A structural entry point may execute a compound structure consisting of `TargetName` followed by its dispatched payload.

2. **AST & IR Model**:
   - `DslPayloadTargetKind` enum: `Sequence`, `Structure`.
   - `DslPayloadDispatch`: `targetKind`, `targetName`, `viewKind`, `controllerFieldName`, `cases`.
   - `DslTypedPayloadDispatch`: `targetKind`, `targetIndex` (struct index or scan index), `controllerFieldIndex`, `viewKind`, `cases` (`std::vector<DslTypedPayloadCase>`).

3. **Compiler Validation**:
   - `TargetName` must match a declared struct in `program.structs` (or a scan in `program.scans`).
   - `controllerFieldName` must be an unsigned bits field in `TargetName`.
   - All case targets must be valid declared structs or `empty`.
   - Duplicate cases are rejected as `DuplicateName`.

---

### 2. Unified Tree Hierarchy and Coordinate Attribution

When executing a structural entrypoint with payload dispatch:

1. **Header Phase**:
   - The root AST node is created for the header structure (e.g. `NalUnit`).
   - Header fields (`forbidden_zero_bit`, `nal_ref_idc`, `nal_unit_type`) are decoded into the root node.
   - Their `FieldLocation` spans map directly to the initial bytes of the input `SourceMapping`.

2. **Dispatch & Transformation Phase**:
   - The runner extracts the scalar value of `controller_field` from the decoded header fields.
   - Remaining bytes in the input view after the header (from `headerBitLength` to `sourceMapping.logicalBitLength()`) form the payload slice.
   - If `view_kind` is specified (e.g. `rbsp`), the runner creates a transformed `SourceMapping` for the payload slice (e.g. excluding emulation prevention bytes), mapping each logical payload bit back to the root source physical spans.

3. **Payload Phase**:
   - The matched payload structure is decoded using the transformed `SourceMapping`.
   - The payload structure AST node is attached as a child under the root header node.
   - All payload field coordinates correctly reflect physical root source coordinates via the transformed mapping.

4. **Partial Tree & Diagnostic Contract**:
   - If header decoding fails: the tree contains header fields decoded prior to failure with attached diagnostics; no payload is executed.
   - If payload decoding fails (truncation, syntax error, missing dependency): the root node retains the materialized header, and the payload child node records the partial state and diagnostic.

---

### 3. Transformed Source Views and Boundary Error Handling

1. **Transformation Provider**:
   - Generalize the unescaping logic from `H264EbspRbspMapper` into a reusable, format-neutral view transformation interface (`PayloadTransformProvider` / `PayloadTransformRegistry`).
   - Supported built-in transform kinds: `None` (identity slice), `Rbsp` (H.264 / H.265 emulation prevention byte unescaping).

2. **Span and Coordinate Invariants**:
   - Input `SourceMapping` must be byte-aligned and non-empty.
   - Transformed mapping preserves excluded spans (`0x03` bytes) so that raw byte selection accurately highlights non-contiguous physical spans.
   - Arithmetic on bit offsets is strictly checked against `std::numeric_limits<quint64>::max()` to prevent coordinate overflow.

3. **Error Propagation**:
   - `EndOfSource` / truncation during payload decoding triggers `DslExecutionStatus::TruncatedSource`.
   - Fault-injected `SourceReadStatus::Error` triggers `DslExecutionStatus::SourceError`.
   - Cancellation token checks occur before header execution, before transformation mapping, and during VM instruction loops.
   - Resource limits (step budget) are enforced across header + payload execution.

---

### 4. Session-Owned Context Lifecycle Management

1. **Session Ownership Principle**:
   - Context directories cannot be passed as unmanaged pointers to stateless runners.
   - `RuleExecutionSession` is promoted to the standard engine for stateful rule execution in both streaming and structural contexts.
   - `AnalysisSession` (or the navigation controller) owns a `RuleExecutionSession` per active format stream.

2. **Context Lifecycle Across Structural Executions**:
   - When a producer structure (e.g. SPS) executes successfully, `RuleExecutionSession` publishes its context definition (e.g. `seq_parameter_set_id`) and stores the exported values in its internal `ContextDirectory`.
   - When a subsequent consumer structure (e.g. PPS) executes within the same session, `RuleExecutionSession` resolves the context import from the previously published definition.
   - If a consumer executes before a producer (e.g. PPS before SPS), `RuleExecutionSession` returns `DependencyUnavailable` and attaches an explanatory diagnostic.
   - Context is scoped by `contextScopeId` and reset on session clear or navigation stack reset.

---

### 5. Rule Package Entrypoint Target Resolution

We evaluate four potential reuse options for multi-entrypoint packages:

1. **Option 1: Multiple `entry` statements in single `.svfmt`**:
   - Causes grammar ambiguity over which entry is active without target naming.
2. **Option 2: Manifest Target Selection (`rule.toml` entry selection)**:
   - `rule.toml` `entrypoints` explicitly specify `entry = "StructName"` or `entry = "SequenceName"` within a shared `.svfmt` path.
   - The compiler compiles the entire `.svfmt` and resolves the requested entrypoint target directly from `program.structs` or `program.scans`.
   - If `entry` is omitted in `rule.toml`, it falls back to the file's default `entry` declaration.
3. **Option 3: Separate source files (`path = "src/h264_nal.svfmt"`)**:
   - Duplicates all 1200+ lines of H.264 syntax definitions. High maintenance burden and drift risk.
4. **Option 4: Cross-file struct module imports**:
   - Excessive compiler complexity for v0.1.

**Decision**: Adopt **Option 2 (Manifest Target Selection)**.
This allows `org.streamview.h264` to expose both `video.h264.annex-b` (sequence entry `nal_units`) and `video.h264.nal` (structural entry `NalUnit`) from the single shared `src/h264_annex_b.svfmt` file with zero code duplication.

---

### 6. Progressive Annex-B Runner Convergence

- The progressive `H264AnnexBAnalyzer` and the structural execution path will share the exact same `RuleExecutionSession` and payload transformation routines.
- No parallel or divergent EBSP/RBSP mapping logic will exist.
- Existing Annex B parsing behavior and test suites will experience zero regression.

---

### 7. Safety, Complexity Bounding, and Format Neutrality

- **Strictly Bounded Execution**: Structural payload dispatch is strictly single-level (one header + at most one payload structure). General recursive call stacks are forbidden.
- **Format-Neutral Runtime**: The C++ runtime contains zero format-specific names (`nal_unit_type`, `FourCC`, `avcC`, `SPS`, `PPS`, `H264`). All format semantics reside purely in `.svfmt` declarative rules.
- **Bounded Transformation Language**: Transformations are selected from a fixed enumeration of robust, tested mapping kinds (`None`, `Rbsp`).

---

## Verification Matrix

| Test Identifier | Category | Input Fixture / Condition | Execution Path | Expected Status | Key Assertions |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `test_parser_structural_payload_dispatch` | DSL Parser / IR | Valid and malformed `payload<rbsp> Struct switch` declarations | `DslCompiler::compile` | Success for valid; specific diagnostics for invalid | AST and Typed IR correctly encode `DslPayloadTargetKind::Structure`, controller index, and cases. |
| `test_structural_payload_header_and_payload_success` | Core Execution | Synthetic NAL fixture with header + SPS-like payload | `RuleExecutionSession` structural execution | `Materialized` | Header fields and payload fields present in single tree; coordinates match exact physical source spans. |
| `test_structural_payload_ebsp_excluded_spans` | Transformation | Synthetic NAL payload containing `0x00000301` | Transformed view execution | `Materialized` | Emulation prevention byte excluded from logical length; payload fields mapped across excluded span. |
| `test_structural_payload_empty_and_default_cases` | Edge Cases | Controller matching `empty` case and unhandled case | Structural payload runner | `Materialized` / `Unsupported` | Empty case produces materialized header with no payload child; unhandled case emits unsupported diagnostic. |
| `test_structural_payload_truncation_and_error` | Fault Injection | Truncated payload bytes and I/O error injected source | Structural payload runner | `TruncatedSource` / `SourceError` | Partial tree retains header; payload node marked with matching diagnostic and state. |
| `test_session_context_producer_consumer_chain` | Context Lifecycle | Sequential execution of SPS producer followed by PPS consumer in same `RuleExecutionSession` | `RuleExecutionSession::run` on SPS then PPS | `Materialized` on both | PPS imports SPS context values (`profile_idc`, `chroma_format_idc`) successfully. |
| `test_session_context_missing_dependency` | Context Lifecycle | Execution of PPS consumer without prior SPS producer | `RuleExecutionSession::run` on PPS alone | `DependencyUnavailable` | Returns `DependencyUnavailable` status with diagnostic identifying missing context ID. |
| `test_manifest_target_resolution` | Package / Manifest | `rule.toml` declaring multiple entrypoints with distinct `entry` targets | `RulePackageCatalog::resolveByFormat` | `Found` for each target | Selected entrypoint resolves and compiles target struct or sequence without syntax duplication. |
| `test_h264_annex_b_zero_regression` | Regression | Full `tests/rules/h264_annex_b_analyzer_test.cpp` suite | `H264AnnexBAnalyzer` on Annex-B bitstream | `Success` (all tests pass) | Full 125/125 test suite passes with identical AST output. |
| `test_aac_mp4_zero_regression` | Regression | All AAC and MP4 analyzer test suites | `AacAdtsAnalyzer`, `Mp4IsobmffAnalyzer` | `Success` (all tests pass) | Zero regressions across all official formats. |

---

## Consequences

### Positive
- **Format-Neutral Modularity**: Enables decoding of MP4 codec configuration payloads (`avcC` SPS/PPS) through clean declarative DSL rules without C++ format coupling.
- **Zero Syntax Duplication**: Manifest target resolution allows `video.h264.annex-b` and `video.h264.nal` to share the same 1200+ lines of H.264 syntax in `src/h264_annex_b.svfmt`.
- **Deterministic Context Propagation**: `RuleExecutionSession` provides audited, session-managed SPS->PPS context resolution without polluting stateless runners.
- **Unified Transformation Engine**: Eliminates dual implementations of EBSP/RBSP mapping.

### Negative / Trade-offs
- Requires extending DSL parser, IR, and compiler to support structural `payload` declarations.
- Requires updating `RulePackageCatalog` and `RuleExecutionSession` to support manifest entry target selection and structural execution.
