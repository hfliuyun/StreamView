# ADR-0086: Ambient Context Imports and Active Parameter Set Resolution

- **Status**: Accepted
- **Date**: 2026-08-15
- **Deciders**: StreamView Core Team

## Context

In H.264 Annex B bitstreams, Supplemental Enhancement Information (SEI) NAL units (`nal_unit_type == 6`) contain one or more SEI messages. While some messages (such as `buffering_period`, `payload_type == 0`) explicitly start with an in-payload key like `ue seq_parameter_set_id` (addressed via locally scoped import keys in ADR-0084), others do not carry any parameter set identifier in their payload.

Specifically, the **Picture Timing SEI message** (`payload_type == 1`, ITU-T H.264 clauses 7.3.2.3.1, D.1.2, and D.2.2) contains:
- `cpb_removal_delay` with dynamic bit length `cpb_removal_delay_length_minus1 + 1`;
- `dpb_output_delay` with dynamic bit length `dpb_output_delay_length_minus1 + 1`;
- conditional fields controlled by `CpbDpbDelaysPresentFlag`, `pic_struct_present_flag`, and `time_offset_length`.

According to clause D.2.2, all these HRD and VUI parameters are governed by the **active Sequence Parameter Set (active SPS)**. Because `pic_timing` has no `seq_parameter_set_id` in its payload, and SEI NAL units appear before primary coded slice NAL units in linear stream order, a forward stream parser must resolve the active SPS from the ambient stream context.

### Explored Dead Ends and Probes

Before defining the ambient import capability, four alternative approaches and parser gates were probed:

1. **Gate 1: Annotation Arity Gate (`src/rules/dsl_ir.cpp:1125`)**:
   Attempting keyless `@context_import("h264-sps")` was rejected by the DSL compiler:
   ```
   error: @context_import requires a context-kind string and a field name
   ```
   Evidence: [`src/rules/dsl_ir.cpp:1125`](file:///Users/yun/code/streamview/src/rules/dsl_ir.cpp#L1120-L1130) strictly mandated two annotation arguments (`kind` and `identifier`).

2. **Gate 2: Parser Expression Arity Gate (`src/rules/dsl.cpp:1892`)**:
   Attempting two-argument `context_value(h264_sps, property)` was rejected by the DSL parser:
   ```
   error: context_value requires three identifier arguments
   ```
   Evidence: [`src/rules/dsl.cpp:1892`](file:///Users/yun/code/streamview/src/rules/dsl.cpp#L1885-L1897) strictly enforced that `context_value` must have three identifier arguments.

3. **Dead End A: Block-Local Constant Key (`computed<u64> assumed_sps_id = 0`)**:
   Under ADR-0084, declaring a block-local computed field `computed<u64> assumed_sps_id = 0;` and using `context_value(assumed_sps_id, h264_sps, ...)` compiles successfully (`svtool rule check` Rule OK). However, this was **rejected** on design and correctness grounds:
   - Literal `0` is not a semantically valid approximation of the active SPS in multi-SPS bitstreams.
   - If a bitstream uses SPS ID 1 or 2 as active, assuming ID 0 will silently misdecode bitfields, read incorrect variable bit lengths, and mark corrupted syntax trees as `Materialized`.

4. **Dead End B: Cross-Case Fallback Binding (`computed<u64> k = optional_value(seq_parameter_set_id, 0)`)**:
   Declaring `ue seq_parameter_set_id` in `case 0:` and referencing `optional_value(seq_parameter_set_id, 0)` in `case 1:` compiles, but `seq_parameter_set_id` is statically bound to the `case 0` execution slot. When evaluating `case 1:` in the same repeat iteration, the slot is unmaterialized, so it unconditionally evaluates to `0`. This degenerates to Dead End A while introducing fragile, order-dependent slot semantics across mutually exclusive switch branches.

## Decision

We introduce **Ambient Context Imports** (`@context_import` without an import key and two-argument `context_value`) to allow structures to read exported properties from the most recent active generation of a given context kind.

### 1. Semantics: Position-Based Ambient Resolution

An ambient context value expression `context_value(kind, property)` resolves to the exported property of the **most recent generation of `(kind, scopeId)` successfully registered strictly prior to the current stream bit position** in `ContextDirectory`. The `scopeId` source is identical to the keyed import resolution path.

This resolution is isomorphic to the position-based generation selection specified in ADR-0078, omitting only the key-equality constraint. Only definitions with `ContextRegistrationStatus::Registered` are considered, and dependency resolution follows existing keyed path semantics.

### 2. Bounded Linear Scan Approximation & Known Limitations

Per ITU-T H.264 clause 7.4.1.2.3 and D.2.2, the normative active SPS is activated by the primary coded picture of the access unit. In our forward-scanning, single-pass analyzer architecture:
- Ambient resolution approximates the active SPS by selecting the latest valid SPS generation preceding the SEI NAL unit in stream position.
- **Known Limitation**: In bitstreams with multiple concurrent SPS definitions where a `buffering_period` SEI message activates an SPS other than the most recently defined one, ambient resolution will select the most recently registered SPS rather than the buffering-period-activated one.
- Full access-unit state machines, cross-NAL lookahead, and intra-struct context re-publication are explicitly **out of scope** for v0.1 and deferred to future access-unit lifecycle milestones.

### 3. Syntax and Grammar Specification

1. **Structure Annotation**:
   ```dsl
   @context_import("h264-sps")
   struct SeiRbsp { ... }
   ```
   The annotation accepts a single string argument specifying the context kind to import in ambient mode.

2. **Context Value Expression**:
   ```dsl
   computed<u64> nal_hrd_present = context_value(h264_sps, effective_nal_hrd_parameters_present_flag);
   ```
   The expression accepts two identifier arguments: the context kind identifier and the exported property name.

3. **Compiler Grammar Relaxations**:
   - `src/rules/dsl_ir.cpp:1125`: Relax `parseContextAnnotation` to accept 1 argument (kind string) for ambient mode (`@context_import("kind")`), in addition to 2 arguments (kind string + field identifier) for keyed mode (`@context_import("kind", key)`).
   - `src/rules/dsl.cpp:1892`: Relax the parser arity check to accept both 2 identifier arguments (ambient form `context_value(kind, field)`) and 3 identifier arguments (keyed form `context_value(key, kind, field)`).
   - `src/rules/dsl_ir.cpp:1508`: Relax IR lowering in `resolveContextValue` to support 2-operand invocations, matching an ambient `@context_import` on the enclosing structure. (Error string `context_value requires an import key, a context-kind identifier, and an exported field name` is updated accordingly).
   - *Note*: `src/rules/dsl_ir.cpp:1553` (the ADR-0084 branch-guarantee domination gate for keyed imports) remains unmodified for keyed operations and is simply bypassed for keyless ambient calls.

### 4. Coexistence with Keyed Imports

- **Coexistence Validity**: A structure may declare both keyed and ambient imports for the same context kind simultaneously (e.g. `SeiRbsp` declares `@context_import("h264-sps", seq_parameter_set_id)` for `case 0:` and `@context_import("h264-sps")` for `case 1:`).
- **Disambiguation Rules**:
  - A 3-argument invocation `context_value(key, kind, field)` binds to the keyed context import matching `key`.
  - A 2-argument invocation `context_value(kind, field)` binds to the ambient context import matching `kind`.
- **Validation Constraints**: Declaring duplicate ambient imports for the same context kind on the same structure is a compile-time error.
- The Task T11b capability test suite will include positive and negative test cases covering coexistence.

### 5. Failure Granularity and Continuation Contract

Following the failure isolation principles of ADR-0084 and aligning with core `ContextLookupStatus` enums:
- If no generation of the ambient `(kind, scopeId)` exists strictly prior to the stream bit position, lookup returns `ContextLookupStatus::NotFound`.
- If an ambient generation is found but one of its transitive dependencies fails to resolve, lookup returns `ContextLookupStatus::DependencyUnavailable`.
- In either failure case, the failure is strictly isolated to the specific SEI message being parsed. The message node is marked with state `MaterializationState::Invalid` / `MaterializationState::WaitingDependency`, and parsing continues at the next SEI message in the same `SeiRbsp` based on `payload_size`.
- Ambient lookup failure never aborts or invalidates the enclosing `SeiRbsp` container.

### 6. Format-Agnostic Core Layer and Complexity

1. **Core Directory Interface**:
   `ContextDirectory` will be extended with a generic query method:
   ```cpp
   [[nodiscard]] ContextLookupResult resolveLatestBefore(ContextDefinitionKind kind,
                                                        quint64 scopeId,
                                                        SourceBitAddress sourcePosition) const;
   ```
2. **Search Mechanics & Complexity**:
   - The lookup queries across the contiguous key range of `(kind, scopeId)` in the existing `definitionsByKey_` index (`std::map<ContextKey, std::vector<ContextDefinitionId>>`, [`src/core/include/streamview/core/context_directory.h:109`](file:///Users/yun/code/streamview/src/core/include/streamview/core/context_directory.h#L105-L113)).
   - For `h264-sps`, the number of keys $K \le 32$ (bounded by `seq_parameter_set_id @range(0, 31)`).
   - Time complexity is $O(K \cdot \log M)$, where $M$ is the average number of generations per key, requiring zero new index state and zero additional memory allocations.
3. **Purity**: Pure read-only query against existing immutable generation records; introduces zero mutable state across analyzer batches.

## Phased Implementation Sequence

To maintain rule consumption momentum and ensure clean capability isolation, work is sequenced as follows:

1. **Task T11a (Current)**: Bilingual ADR-0086 specification and probe archival (Markdown-only).
2. **Task T12a**: Frame packing arrangement SEI message decoding (`payload_type == 45`, package version `0.1.36`) — pure rule consumption, no context dependency.
3. **Task T12b**: Display orientation SEI message decoding (`payload_type == 47`, package version `0.1.37`) — pure rule consumption, no context dependency.
4. **Task T11b**: Ambient context import engine capability (`ContextDirectory::resolveLatestBefore`, `dsl_ir`, `dsl_vm`, targeted unit tests) — pure capability slice, package version unchanged.
5. **Task T11c**: Pic timing SEI message decoding (`payload_type == 1`, package version `0.1.38`) — rule consumption slice exporting required SPS parameters and consuming ambient SPS context.

## References

- ITU-T H.264 Clauses 7.3.2.3.1, 7.4.1.2.3, D.1.2, D.2.2
- ADR-0078: Bind Redefined Parameter Sets By Stream Position
- ADR-0084: Locally Scoped Context Import Keys
- ADR-0085: Decode the Buffering Period SEI Message
