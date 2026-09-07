# ADR-0112: Rule Package Store, Format Detectors, and MP4 Sample Extractor Fuzzing Harness

- **Status**: Proposed
- **Date**: 2026-09-07
- **Authors**: StreamView Contributors

---

## Context

Following the establishment of the core DSL fuzzing harness in ADR-0111 (covering `DslParser`, `DslCompiler`, and `DslVirtualMachine`), StreamView must harden its secondary external ingestion boundaries:
1. **Rule Package Distribution and Storage (`RulePackageStore`)**: Ingests external `.svrule` ZIP archives and directories containing untrusted file paths, manifests (`rule.toml`), and format schemas;
2. **Format Detectors and Demuxing Scanners (`H264AnnexBDetector`, `H264StartCodeScanner`, `AacAdtsDetector`, `AacAdtsScanner`, `Mp4BoxDetector`, `Mp4BoxScanner`)**: Ingests raw untrusted byte streams from arbitrary files to determine format identity and slice streaming records;
3. **MP4 Sample Table Extractor and Indexing Engine (`Mp4SampleTableExtractor`, `Mp4SampleTableIndex`)**: Reconstructs chunk-to-sample indexing, composition timestamps, sync points, and sample bounds from potentially pathological ISOBMFF `stbl` structures.

Per ADR-0110 Section 1.1 and 1.2, these three critical subsystems represent untrusted binary parsing surfaces where malformed inputs, malicious archives (Zip Slip, zip bombs), integer multiplication wraps, cyclic container references, and false syncword cascades could trigger denial-of-service, crashes, or memory corruption.

Furthermore, ADR-0110 Section 1.2 mandates that rule package and format scanner fuzzing corpora explicitly incorporate synthetic syntax trees whose field and node names contain `#<digits>` (e.g., `tag#1`, `entry#0`) to verify path resolution robustness and confirm zero collision with `#<row>` sibling disambiguation.

---

## Decisions

### 1. Dual-Mode Extension and Harness Architecture

**Decision**: Extend the dual-mode architecture established in ADR-0111 (`STREAMVIEW_ENABLE_LIBFUZZER` vs. standalone deterministic playback via `standalone_fuzz_driver.h`) to three new targets:
1. `fuzz_rule_package_store`: targets archive extraction, directory import, and manifest parsing;
2. `fuzz_format_detectors`: targets H.264 Annex B, AAC ADTS, and MP4 box detection and streaming scanners;
3. `fuzz_mp4_sample_extractor`: targets ISOBMFF sample table tree extraction and index building.

All targets export the standard entry point `extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)` and link against `standalone_fuzz_driver.h` when `STREAMVIEW_ENABLE_LIBFUZZER` is disabled.

---

### 2. Rule Package Store Fuzz Target (`fuzz_rule_package_store`)

**Decision**: Implement `fuzz_rule_package_store` targeting `streamview::rules::RulePackageStore::importArchive` and manifest validation.

- **Input Ingestion**:
  - Treats input data as an untrusted binary `.svrule` (ZIP archive).
  - Writes input to a temporary file via Qt's `QTemporaryFile` and invokes `RulePackageStore::importArchive(tempFile.fileName())`.
  - If the archive successfully imports, verifies that `RulePackageStore::writeArchive` and `RulePackageStore::importDirectory` operate deterministically without error.
- **Threat Model & Invariants**:
  1. **Zip Slip Defense**: Any archive containing path components with `..`, absolute paths, leading slashes, Windows drive prefixes, or reserved characters must be rejected with `RulePackageImportStatus::InvalidArchive` or `InvalidPackage`. No file may ever be extracted outside the designated staging directory;
  2. **Zip Bomb and Resource Bounds Defense**:
     - Archives exceeding `maximumArchiveBytes = 64 MiB` must be rejected immediately;
     - Uncompressed files exceeding `maximumFileBytes = 8 MiB` or total extracted bytes exceeding `maximumTotalBytes = 64 MiB` must be aborted cleanly;
     - Path length exceeding `maximumPathBytes = 240` must be rejected;
     - Manifest (`rule.toml`) exceeding `maximumManifestBytes = 64 KiB` must be rejected;
  3. **Manifest and Syntax Robustness**: Pathological TOML keys, circular dependencies, invalid version strings, and syntax trees whose field/node names contain `#<digits>` (e.g., `tag#1`, `entry#0`) must be parsed safely and never corrupt package identity or path resolution;
  4. **Zero Crash / Leak Guarantee**: Corrupted Central Directory structures, invalid compression methods, and CRC mismatches must return explicit failure statuses without crashing.

---

### 3. Format Detectors and Scanners Fuzz Target (`fuzz_format_detectors`)

**Decision**: Implement `fuzz_format_detectors` targeting candidate detection and batch streaming across H.264 Annex B, AAC ADTS, and MP4 box scanners.

- **Input Ingestion**:
  - Feeds arbitrary raw byte buffers (up to 4 MiB) through:
    1. `detectH264AnnexBCandidate` and `H264StartCodeScanner::scanBatch`;
    2. `detectAacAdtsCandidate` and `AacAdtsScanner::scanBatch`;
    3. `detectMp4Candidate` and `Mp4BoxScanner::scanBatch`.
  - In-memory data sources (`MemorySource`) backed by `std::span<const std::byte>` emulate real-world random-access sources without disk I/O.
- **Threat Model & Invariants**:
  1. **Zero Hang / Bounded Execution**:
     - False syncword storms (e.g., repeating `0xFF 0xFF` or `0x00 0x00 0x01` patterns) and cyclic MP4 box hierarchies (e.g., `boxSize = 0`, self-referential box offsets) must respect `defaultWorkBudget()` (64 KiB) and maximum batch records (256), completing within deterministic bounds;
  2. **Zero Out-of-Bounds Memory Reads**:
     - Detectors operating on `std::span<const std::byte>` and scanners operating on `core::RandomAccessSource` must strictly check boundaries on every byte/word read; truncated frames or boxes must be marked with `truncated = true` or cleanly finished without reading beyond `sourceSizeBytes`;
  3. **Deterministic Classification**:
     - Repeated invocation on the same input buffer must yield identical confidence scores and scan records.

---

### 4. MP4 Sample Table Extractor and Indexing Fuzz Target (`fuzz_mp4_sample_extractor`)

**Decision**: Implement `fuzz_mp4_sample_extractor` targeting `streamview::rules::Mp4SampleTableIndex::build` and `streamview::rules::Mp4SampleTableExtractor`.

- **Input Ingestion**:
  - Deserializes arbitrary fuzzer bytes into structured table inputs:
    - Run-length rows: `stts` (time-to-sample), `stsc` (sample-to-chunk), `ctts` (composition offset);
    - Chunk offset entries (`stco` / `co64`);
    - Sample size entries (`stsz` / `stz2`);
    - Sync sample entries (`stss`).
  - Calls `Mp4SampleTableIndex::build` against synthetic table readers and executes `requestPage` on boundary pages (e.g., page 0, middle, out-of-bounds).
  - Also feeds serialized box structures through `Mp4IsobmffAnalyzer` and `Mp4SampleTableExtractor::extract`.
- **Threat Model & Invariants**:
  1. **Integer Wrap and Overflow Protection**:
     - Checked arithmetic (`__builtin_mul_overflow`, checked additions) must prevent integer overflow during chunk-to-sample calculations (`firstChunk`, `samplesPerChunk`), timestamp accumulations (`sampleDelta`), and source span offsets; any wrap must fail cleanly with `Mp4SampleTableIndexStatus::ArithmeticOverflow`;
  2. **Memory and Row Budget Enforced**:
     - Declarations exceeding `maximumRunRows = 65,536` or table read budgets must immediately trigger `ResourceLimit` or `InconsistentTables`, preventing OOM on astronomical entry counts;
  3. **Range and Overlap Safety**:
     - Descriptors resolving outside `sourceSizeBytes` or overlapping in the bitstream must cleanly report `OutOfSourceRange`.

---

### 5. Seed Corpora Organization and CTest Integration

**Decision**: Provide comprehensive seed corpora in `tests/fixtures/fuzz/` covering valid, borderline, and pathological inputs:
- `tests/fixtures/fuzz/rule_package/`:
  - 10 seed files covering valid `.svrule` packages, zip bombs (nested compression), Zip Slip traversal attempts (`../../etc/passwd`), corrupt manifests, truncated archives, and synthetic schemas containing `#<digits>` field names;
- `tests/fixtures/fuzz/format_detectors/`:
  - 10 seed files covering valid H.264 NAL streams, valid AAC ADTS audio, valid MP4 headers, dense false syncwords, cyclic box loops, 64-bit largesize boxes, and zero-length/truncated inputs;
- `tests/fixtures/fuzz/mp4_sample_extractor/`:
  - 10 seed files covering standard sample tables, integer overflow triggers, single-sample tracks, large run rows, non-monotonic composition timestamps, and overlapping chunk offsets.

**CMake / CTest Target Registration**:
- `streamview_fuzz_rule_package_store`
- `streamview_fuzz_format_detectors`
- `streamview_fuzz_mp4_sample_extractor`
- Registered with CTest; total test suite count increases from 56 to 59.

---

## Consequences

### Positive
- Closes the untrusted binary ingestion attack surface outside the core DSL engine.
- Hardens archive handling against directory traversal and archive decompression bombs.
- Guarantees streaming scanners and candidate detectors never hang on corrupt, unaligned, or adversarial bitstreams.
- Verifies path disambiguation isolation by fuzzing schemas containing `#<digits>`.
- Fully automated regression checking in standard CTest across Ubuntu, macOS, and Windows.

### Negative
- CTest suite expands by 3 tests; additional build and execution time (~2-3 seconds).
