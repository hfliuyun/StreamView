# ADR-0096: MP4/ISOBMFF Container Architecture, Box Traversal, Cross-Layer Navigation, and Sample Indexing Boundaries

- **Status**: Proposed
- **Date**: 2026-08-16
- **Authors**: StreamView Contributors

---

## Context

Phase 5 of StreamView introduces non-fragmented MP4/ISOBMFF (`video/mp4`, `audio/mp4`, ISO/IEC 14496-12 / 14496-14 / 14496-15) container parsing, metadata tree inspection, sample table index mapping, and cross-layer navigation to underlying elementary streams (H.264 NAL units in `avcC` and AAC ASC in `esds`).

Unlike H.264 Annex B and AAC ADTS streams—which rely on continuous synchronization byte patterns (`00 00 01` / `0x7FF`)—ISOBMFF containers are structured as a hierarchical tree of length-prefixed boxes (atoms). Furthermore, media data payloads (`mdat`) can range from megabytes to hundreds of gigabytes, and sample index tables (`stts`, `stsc`, `stsz`, `stco`, `co64`) contain tens or hundreds of thousands of entries.

To prevent architectural regressions, this ADR establishes the definitive boundaries for:
1. **D1: Box traversal and `mdat` lazy encapsulation responsibility** (Core vs DSL);
2. **D2: Cross-layer navigation and inter-rule reference model** (`avcC`/`esds` $\to$ H.264/AAC);
3. **D3: Sample table indexing and resource budgets** (`stsc`/`stsz`/`stco`/`co64`).

---

## Decision 1: Box Traversal, Hierarchy, and `mdat` Lazy Boundary (D1)

### Conclusion
1. **Format Neutrality in Core**: Per the global architectural rule, no format-specific strings, box FourCC identifiers (`ftyp`, `moov`, `mdat`, `trak`, `stbl`), or box taxonomies shall be introduced into `src/core/` or `src/rules/*.cpp`.
2. **DSL Structured Box Model**: All ISOBMFF box headers, box hierarchies, and payload schemas reside strictly within the official MP4 rule package (`org.streamview.mp4`).
3. **Box Header Specification**: Every box begins with a standard 8-byte header consisting of `bits<32> size;` and `bits<32> box_type;`. FourCC matching is performed via 32-bit integer literals:
   - `ftyp`: `0x66747970`
   - `moov`: `0x6D6F6F76`
   - `mdat`: `0x6D646174`
   - `trak`: `0x7472616B`
   - `stbl`: `0x7374626C`
   - `avcC`: `0x61766343`
   - `esds`: `0x65736473`
   - `moof`: `0x6D6F6F66`
4. **`mdat` Lazy Regioning & Largesize Representation**: Media data (`mdat`) is declared in DSL with `@lazy` byte payloads scoped within branch-local computed dependencies:
   ```svfmt
   if (size == 1) {
       bits<64> largesize;
       computed<u64> large_payload = largesize - 16;
       @lazy(large_payload) bytes large_data;
   } else {
       computed<u64> payload = size - 8;
       @lazy(payload) bytes data;
   }
   ```
   Large media payloads are never eagerly loaded into memory, consuming 0 heap for samples while preserving exact physical and logical bit ranges.
5. **Open Language Capability Gaps**:
   - `size == 0` (extends to EOF): Currently unexpressible in DSL without language enhancements because `compressed_payload` is restricted to the final top-level item and cannot enter an `if` branch, and no built-in remaining span expression exists. This is documented as an open language capability gap to be resolved in Task P5c / ADR-0097.
   - `type == 'uuid'` (extended type): Pending string literal tokenizer / byte array matching scoping in ADR-0097.

### Evidence & Basis
- `src/core/include/streamview/core/analysis_model.h:38-42`: `MaterializationState::Lazy` is already supported by the analysis tree and cache payload engine (`analysis_cache_payload.cpp:277`).
- `src/rules/official/org.streamview.aac/src/aac_adts.svfmt:24-27`: Successfully demonstrated `@lazy` payload encapsulation in Phase 4 (`raw_data_block`).

### Rejected Alternatives
- **Hardcoded C++ Box Scanner**: Embedding FourCC dispatch (`switch (fourcc)`) in C++.
  - *Rejection Reason*: Violates the global ban on format-specific logic in the core engine.
- **Pure DSL Unbounded Recursion without Lazy Encapsulation**: Eagerly materializing all box contents including `mdat`.
  - *Rejection Reason*: Exceeds compile-time structure limits (`maximumExpandedFieldsPerStructure = 99'999` in `src/rules/dsl_ir.cpp:13`) and runtime node limits (`defaultMaximumMaterializedNodes() = 100'000` in `src/rules/include/streamview/rules/dsl_vm.h:36-38`), exhausting memory on multi-gigabyte media streams.

---

## Decision 2: Cross-Layer Navigation and Inter-Rule Reference Model (D2)

### Conclusion
1. **Self-Contained Rule Packages**: In v0.1, rule packages remain self-contained. The `org.streamview.mp4` package specifies container syntax, descriptor structures (`ES_Descriptor`, `DecoderConfigDescriptor`), and decoder configuration records (`AVCDecoderConfigurationRecord`).
2. **Semantic Linking via Target Format Annotations**: Codec configuration boxes export structured byte regions annotated with metadata (e.g. `@target_format("video.h264.nal", "SPS")` and `@target_format("audio.aac.asc", "AudioSpecificConfig")`).
3. **Session-Level Cross-Layer Navigation**: When a user or automated inspector interacts with an `avcC` or `esds` node, the `AnalysisSession` / UI layer queries the `RulePackageCatalog` for the registered target package entry point (`RulePackageCatalog::resolve` in `src/rules/include/streamview/rules/rule_catalog.h:52`) and spawns an auxiliary coordinate-mapped view over that byte span.
4. **Future Extension**: Full compile-time inter-package dependency management (`dependencies = [...]` in `rule.toml`) will be introduced as an independent DSL compiler capability slice in Phase 7+.

### Evidence & Basis
- `src/rules/include/streamview/rules/rule_catalog.h:52`: `RulePackageCatalog::resolve(identity, entryPointId, runningLanguage, runningEngine)` provides entry-point lookup across installed packages.
- `src/rules/dsl_vm.cpp:1113-1117`: `MaterializationState::WaitingDependency` is available if asynchronous cross-layer dependencies are required.

### Rejected Alternatives
- **Dynamic Inter-Package AST Linker in v0.1**: Introducing cross-package symbol imports before compiler module semantics are defined.
  - *Rejection Reason*: High architectural risk and scope explosion without proven requirement for basic container inspection.
- **C++ Hardcoded Cross-Analyzer Coupling**: Directly invoking `H264AnnexBAnalyzer` inside `Mp4IsobmffAnalyzer`.
  - *Rejection Reason*: Violates core decoupling and prevents independent testing of container rules.

---

## Decision 3: Sample Table Indexing and Resource Budget Boundaries (D3)

### Conclusion
1. **Windowed Sample Table Inspection as Language-Level Constraint**:
   - Sample table metadata (`entry_count`, table configuration) is fully materialized, while repetitive large index tables (`stsz` entry arrays, `stco`/`co64` chunk offset arrays) are region-bounded using `@lazy` structures and batch iteration.
   - Windowing is not an optional resource optimization, but a mandatory language-level constraint to prevent hitting compile-time expansion limits (`maximumExpandedFieldsPerStructure = 99'999` in `src/rules/dsl_ir.cpp:13`) and runtime node limits (`defaultMaximumMaterializedNodes() = 100'000` in `src/rules/include/streamview/rules/dsl_vm.h:36-38`).
2. **Resource Budgeting (Design Targets, Subject to Task P5j Calibration)**:
   - Maximum batch limit: `maximumRecords = 1000` samples per batch (design target).
   - Materialized node limit: strictly bounded below `defaultMaximumMaterializedNodes() = 100'000`.
   - Memory budget: $< 32\text{ MB}$ per 100,000 indexed samples (design target).
3. **State Integrity**:
   - `MaterializationState::Indexing`: Utilized during container discovery (`src/core/analysis_model.cpp:59`).
   - `MaterializationState::WaitingDependency`: Generated if external track definitions are pending (`src/rules/dsl_vm.cpp:1116`).

### Evidence & Basis
- `src/core/analysis_model.cpp:59`: `MaterializationState::Indexing` is a valid tree node lifecycle state.
- `src/rules/dsl_vm.cpp:1116`: `DslExecutionStatus::DependencyUnavailable \to MaterializationState::WaitingDependency` is fully verified in the core engine.

### Rejected Alternatives
- **Eager Individual Node Allocation per Sample**: Creating an `AnalysisNode` for every sample in a 500,000-sample stream.
  - *Rejection Reason*: Consumes hundreds of megabytes of heap, violates `maximumExpandedFieldsPerStructure`, and degrades UI tree view rendering.

---

## Phase 5 Task Slices and Execution Plan

| Task | Type | Description |
| :--- | :--- | :--- |
| **Task P5a** | Specification | Bilingual ADR-0096 architecture and implementation plan refinement (Markdown-only). |
| **Task P5b** | Capability | DSL compiler unrecognized annotation gate (N2 hardening, reject bogus annotations with compile errors). |
| **Task P5c** | Specification | ISOBMFF container language primitives probing & ADR-0097 (`size==0` EOF span, `@target_format` host placement, box sequence scoping). |
| **Task P5d** | Capability | Container language capabilities implementation (runner & parser primitives). |
| **Task P5e** | Rule | Official MP4 rule package `org.streamview.mp4` v0.1.0 (top-level box traversal, `ftyp`, `mdat` lazy). |
| **Task P5f** | Rule | `moov` container hierarchy v0.1.1 (`moov`, `trak`, `mdia`, `minf`, `stbl`). |
| **Task P5g** | Rule | Sample table paging v0.1.2 (`stts`, `stsc`, `stsz`, `stco`, `co64`). |
| **Task P5h** | Rule | Codec configuration v0.1.3 (`stsd`, `avc1`, `avcC`, `mp4a`, `esds` + `@target_format`). |
| **Task P5i** | Capability | Cross-layer navigation session and coordinate view integration. |
| **Task P5j** | Acceptance | Bit-by-bit acceptance audit, large `mdat` lazy verification, and Phase 5 milestone closure. |
