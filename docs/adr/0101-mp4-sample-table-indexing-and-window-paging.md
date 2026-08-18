# ADR-0101: MP4 Sample Table Indexing and Window Paging Rules v0.1.2

- **Status**: Proposed
- **Date**: 2026-08-18
- **Authors**: StreamView Contributors

---

## Context

Task P5f completed the `moov` container hierarchy and time header rules (`org.streamview.mp4` v0.1.1), establishing recursive container navigation for `moov`, `trak`, `edts`, `mdia`, `minf`, and `stbl`.

Task P5g implements the sample table index and window paging rules in `org.streamview.mp4` v0.1.2:
1. Time-to-Sample Box (`stts` / `0x73747473`): `TimeToSampleEntry` with `@window(TimeToSampleEntry, entry_count)`.
2. Sample-to-Chunk Box (`stsc` / `0x73747363`): `SampleToChunkEntry` with `@window(SampleToChunkEntry, entry_count)`.
3. Sample Size Box (`stsz` / `0x7374737A`): `SampleSizeEntry` with `@window(SampleSizeEntry, sample_count)` when `sample_size == 0`, and uniform `sample_size` scalar when `sample_size > 0`.
4. Chunk Offset Box (`stco` / `0x7374636F`): `ChunkOffsetEntry` (32-bit offset) with `@window(ChunkOffsetEntry, entry_count)`.
5. Chunk Large Offset Box (`co64` / `0x636F3634`): `ChunkLargeOffsetEntry` (64-bit offset) with `@window(ChunkLargeOffsetEntry, entry_count)`.

All 5 sample table boxes must:
- FullBox header decoding (`version`, `flags`) and reject `version != 0` via `unsupported(...)` diagnostics;
- Support standard 32-bit box size, 64-bit `largesize` (`size == 1`), and `size == 0` EOF span;
- Reuse existing DSL `@window` annotation, `WindowDecoder`, `RunnerExecutionBudget`, paged cache, and source coordinate mapping without any format-specific C++ logic;
- Maintain strict boundaries: Codec configuration boxes (`stsd`, `avc1`, `avcC`, `mp4a`, `esds`) remain deferred to Task P5h, and cross-layer navigation remains deferred to Task P5i.

---

## Decision

### 1. Package Manifest (`src/rules/official/org.streamview.mp4/rule.toml`)

The package version is incremented from `0.1.1` to `0.1.2`:

```toml
manifest-version = 1

[package]
id = "org.streamview.mp4"
version = "0.1.2"
authors = ["StreamView contributors"]
license = "MIT"
dependencies = []

[compatibility]
language = "0.1"
engine = ">=0.1.0 <0.2.0"

[[entrypoints]]
id = "main"
format = "video.mp4"
source = "src/mp4_isobmff.svfmt"
profiles = ["isobmff"]
depth = "boxes"
detector = "mp4-box"
```

### 2. DSL Sample Table Entry Structs and Box Schemas

```svfmt
@spec("ISO/IEC 14496-12:2015", "8.6.1.2")
@description("Time to Sample entry.")
struct TimeToSampleEntry {
    bits<32> sample_count
        @description("Number of consecutive samples with this delta.");
    bits<32> sample_delta
        @description("Delta of these samples in timescale units.");
}

@spec("ISO/IEC 14496-12:2015", "8.6.1.2")
@description("Time to Sample box payload.")
struct TimeToSampleBox {
    bits<8> version
        @description("Version (0).");
    bits<24> flags
        @description("Flags.");
    bits<32> entry_count
        @description("Number of time-to-sample entries.");
    if (version == 0) {
        computed<u64> table_bytes = entry_count * 8;
        @lazy(table_bytes) bytes entries @window(TimeToSampleEntry, entry_count);
    } else {
        unsupported("Unsupported stts FullBox version") at version;
    }
}

@spec("ISO/IEC 14496-12:2015", "8.7.4")
@description("Sample to Chunk entry.")
struct SampleToChunkEntry {
    bits<32> first_chunk
        @description("First chunk index using this description.");
    bits<32> samples_per_chunk
        @description("Number of samples in each chunk.");
    bits<32> sample_description_index
        @description("Sample description index for these samples.");
}

@spec("ISO/IEC 14496-12:2015", "8.7.4")
@description("Sample to Chunk box payload.")
struct SampleToChunkBox {
    bits<8> version
        @description("Version (0).");
    bits<24> flags
        @description("Flags.");
    bits<32> entry_count
        @description("Number of sample-to-chunk entries.");
    if (version == 0) {
        computed<u64> table_bytes = entry_count * 12;
        @lazy(table_bytes) bytes entries @window(SampleToChunkEntry, entry_count);
    } else {
        unsupported("Unsupported stsc FullBox version") at version;
    }
}

@spec("ISO/IEC 14496-12:2015", "8.7.3.2")
@description("Sample Size entry.")
struct SampleSizeEntry {
    bits<32> entry_size
        @description("Size of individual sample in bytes.");
}

@spec("ISO/IEC 14496-12:2015", "8.7.3.2")
@description("Sample Size box payload.")
struct SampleSizeBox {
    bits<8> version
        @description("Version (0).");
    bits<24> flags
        @description("Flags.");
    bits<32> sample_size
        @description("Default sample size (if 0, individual sizes follow in table).");
    bits<32> sample_count
        @description("Number of samples.");
    if (version == 0) {
        if (sample_size == 0) {
            computed<u64> table_bytes = sample_count * 4;
            @lazy(table_bytes) bytes entries @window(SampleSizeEntry, sample_count);
        }
    } else {
        unsupported("Unsupported stsz FullBox version") at version;
    }
}

@spec("ISO/IEC 14496-12:2015", "8.7.5")
@description("32-bit Chunk Offset entry.")
struct ChunkOffsetEntry {
    bits<32> chunk_offset
        @description("32-bit byte offset of chunk from beginning of file.");
}

@spec("ISO/IEC 14496-12:2015", "8.7.5")
@description("32-bit Chunk Offset box payload.")
struct ChunkOffsetBox {
    bits<8> version
        @description("Version (0).");
    bits<24> flags
        @description("Flags.");
    bits<32> entry_count
        @description("Number of chunk offset entries.");
    if (version == 0) {
        computed<u64> table_bytes = entry_count * 4;
        @lazy(table_bytes) bytes entries @window(ChunkOffsetEntry, entry_count);
    } else {
        unsupported("Unsupported stco FullBox version") at version;
    }
}

@spec("ISO/IEC 14496-12:2015", "8.7.5")
@description("64-bit Chunk Offset entry.")
struct ChunkLargeOffsetEntry {
    bits<64> chunk_offset
        @description("64-bit byte offset of chunk from beginning of file.");
}

@spec("ISO/IEC 14496-12:2015", "8.7.5")
@description("64-bit Chunk Offset box payload.")
struct ChunkLargeOffsetBox {
    bits<8> version
        @description("Version (0).");
    bits<24> flags
        @description("Flags.");
    bits<32> entry_count
        @description("Number of 64-bit chunk offset entries.");
    if (version == 0) {
        computed<u64> table_bytes = entry_count * 8;
        @lazy(table_bytes) bytes entries @window(ChunkLargeOffsetEntry, entry_count);
    } else {
        unsupported("Unsupported co64 FullBox version") at version;
    }
}
```

### 3. Window Pagination and Execution Semantics

1. **Unconditional Count Field Requirement**:
   - In accordance with ADR-0097 and `dsl_ir.cpp`, count fields (`entry_count`, `sample_count`) are declared unconditionally at the struct level of their respective payload boxes (`TimeToSampleBox`, `SampleToChunkBox`, `SampleSizeBox`, `ChunkOffsetBox`, `ChunkLargeOffsetBox`).
2. **WindowDecoder Integration**:
   - `WindowDecoder` decodes batches of fixed-size entries on demand without eagerly materializing entire multi-megabyte sample tables.
   - Repeated page requests reuse existing cached tree nodes without redundant allocations.
   - Budget constraints in `RunnerExecutionBudget` prevent memory exhaustion on streams with large `entry_count`.
3. **Disjoint Coordinate Mapping**:
   - Entries within windowed byte regions map accurately through `SourceMapping` and `SourceBitAddress`.

---

## Consequences

- Full ISOBMFF sample table indexing (`stts`, `stsc`, `stsz`, `stco`, `co64`) is active through DSL rules.
- Window pagination bounds memory consumption during container analysis.
- P5h codec configuration and P5i cross-layer navigation remain cleanly separated.
