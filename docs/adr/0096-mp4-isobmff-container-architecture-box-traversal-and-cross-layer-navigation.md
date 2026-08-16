# ADR-0096: MP4/ISOBMFF Container Architecture, Box Traversal, Cross-Layer Navigation, and Sample Indexing Boundaries

- **Status**: Accepted
- **Date**: 2026-08-16
- **Authors**: StreamView Core Team

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
3. **Box Header Specification**: Every box begins with a standard 8-byte header (`size: u32`, `type: bytes(4)`). Extended sizes (`size == 1 \to u64 largesize`) and extended types (`type == 'uuid' \to bytes(16) usertype`) are expressed directly in DSL syntax.
4. **`mdat` Lazy Regioning**: Media data (`mdat`) is declared in DSL with `@lazy(payload_bytes) bytes data;`. Large media payloads are never eagerly loaded into memory, consuming 0 heap for samples while preserving exact physical and logical bit ranges.
5. **Edge Cases**:
   - `size == 0` (extends to EOF): Handled via DSL remaining-span expressions (`computed<u64> payload_bytes = available_bytes;`).
   - `size == 1` (64-bit largesize): Handled via `if (size == 1) u64 largesize; computed<u64> payload_bytes = (size == 1 ? largesize - 16 : size - 8);`.

### Evidence & Basis
- `src/core/include/streamview/core/analysis_model.h:38-42`: `MaterializationState::Lazy` is already supported by the analysis tree and cache payload engine (`analysis_cache_payload.cpp:277`).
- `src/rules/official/org.streamview.aac/src/aac_adts.svfmt:24-27`: Successfully demonstrated `@lazy` payload encapsulation in Phase 4 (`raw_data_block`).

### Rejected Alternatives
- **Hardcoded C++ Box Scanner**: Embedding FourCC dispatch (`switch (fourcc)`) in C++.
  - *Rejection Reason*: Violates the global ban on format-specific logic in the core engine.
- **Pure DSL Unbounded Recursion without Lazy Encapsulation**: Eagerly materializing all box contents including `mdat`.
  - *Rejection Reason*: Exceeds `maximumMaterializedNodes` (20,000) and exhausts system memory on multi-gigabyte media streams.

---

## Decision 2: Cross-Layer Navigation and Inter-Rule Reference Model (D2)

### Conclusion
1. **Self-Contained Rule Packages**: In v0.1, rule packages remain self-contained. The `org.streamview.mp4` package specifies container syntax, descriptor structures (`ES_Descriptor`, `DecoderConfigDescriptor`), and decoder configuration records (`AVCDecoderConfigurationRecord`).
2. **Semantic Linking via Target Format Annotations**: Codec configuration boxes export structured byte regions annotated with metadata (e.g. `@target_format("video.h264.nal", "SPS")` and `@target_format("audio.aac.asc", "AudioSpecificConfig")`).
3. **Session-Level Cross-Layer Navigation**: When a user or automated inspector interacts with an `avcC` or `esds` node, the `AnalysisSession` / UI layer queries the `RulePackageStore` for the registered target package entry point (`video.h264.annexb` or `audio.aac.asc`) and spawns an auxiliary coordinate-mapped view over that byte span.
4. **Future Extension**: Full compile-time inter-package dependency management (`dependencies = [...]` in `rule.toml`) will be introduced as an independent DSL compiler capability slice in Phase 7+.

### Evidence & Basis
- `src/rules/include/streamview/rules/rule_package_store.h:35-52`: `RulePackageStore` and `RulePackageCatalog` already provide entry-point resolution across installed packages.
- `src/rules/dsl_vm.cpp:1113-1117`: `MaterializationState::WaitingDependency` is available if asynchronous cross-layer dependencies are required.

### Rejected Alternatives
- **Dynamic Inter-Package AST Linker in v0.1**: Introducing cross-package symbol imports before compiler module semantics are defined.
  - *Rejection Reason*: High architectural risk and scope explosion without proven requirement for basic container inspection.
- **C++ Hardcoded Cross-Analyzer Coupling**: Directly invoking `H264AnnexBAnalyzer` inside `Mp4IsobmffAnalyzer`.
  - *Rejection Reason*: Violates core decoupling and prevents independent testing of container rules.

---

## Decision 3: Sample Table Indexing and Resource Budget Boundaries (D3)

### Conclusion
1. **Windowed Sample Table Inspection**: Sample table metadata (`entry_count`, table configuration) is fully materialized, while repetitive large index tables (`stsz` entry arrays, `stco`/`co64` chunk offset arrays) are region-bounded using `@lazy` structures and batch iteration.
2. **Resource Budgeting**:
   - Maximum batch limit: `maximumRecords = 1000` samples per batch.
   - Materialized node limit: strictly bounded below `maximumMaterializedNodes = 20,000`.
   - Memory budget: $< 32\text{ MB}$ per 100,000 indexed samples.
3. **State Integrity**:
   - `MaterializationState::Indexing`: Utilized during container discovery.
   - `MaterializationState::WaitingDependency`: Generated if external track definitions are pending.

### Evidence & Basis
- `src/core/analysis_model.cpp:59`: `MaterializationState::Indexing` is a valid tree node lifecycle state.
- `src/rules/dsl_vm.cpp:1116`: `DslExecutionStatus::DependencyUnavailable \to MaterializationState::WaitingDependency` is fully verified in the core engine.

### Rejected Alternatives
- **Eager Individual Node Allocation per Sample**: Creating an `AnalysisNode` for every sample in a 500,000-sample stream.
  - *Rejection Reason*: Consumes hundreds of megabytes of heap and degrades UI tree view rendering.

---

## Phase 5 Task Slices and Execution Plan

| Task | Type | Description |
| :--- | :--- | :--- |
| **Task P5a** | Specification | Bilingual ADR-0096 architecture and implementation plan refinement (Markdown-only). |
| **Task P5b** | Capability | DSL compiler / language gate hardening (reject unrecognized annotations with compile errors). |
| **Task P5c** | Rule | Official MP4 rule package `org.streamview.mp4` v0.1.0 (`rule.toml`, `mp4_box.svfmt`, `mp4_ftyp.svfmt`, `mp4_moov.svfmt`, `mp4_trak.svfmt`, `mp4_mdat.svfmt`). |
| **Task P5d** | Capability | `Mp4IsobmffAnalyzer` runner, detector registration, and container traversal tests. |
| **Task P5e** | Rule | Sample table and codec configuration rules (`stsd`, `avc1`, `avcC`, `mp4a`, `esds`, `stts`, `stsc`, `stsz`, `stco`, `co64`). |
| **Task P5f** | Capability | Cross-layer navigation hooks from `avcC` / `esds` to H.264 SPS/PPS and AAC ASC entry points. |
| **Task P5g** | Acceptance | Bit-by-bit acceptance audit, large `mdat` lazy verification, and Phase 5 milestone closure. |
