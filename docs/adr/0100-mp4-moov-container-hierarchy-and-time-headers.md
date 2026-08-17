# ADR-0100: MP4 moov Container Hierarchy and Time Header Rules v0.1.1

- **Status**: Proposed
- **Date**: 2026-08-18
- **Authors**: StreamView Contributors

---

## Context

Task P5e established the official `org.streamview.mp4` rule package (v0.1.0) with top-level box scanning (`ftyp`, `mdat` lazy, `moof` unsupported warning, and unknown opaque box fallback).

Task P5f extends `org.streamview.mp4` to version `0.1.1` by specifying:
1. Recursive container re-entry via `@container(Box)` for all container boxes:
   - Movie Box (`moov` / `0x6D6F6F76`)
   - Track Box (`trak` / `0x7472616B`)
   - Edit Box (`edts` / `0x65647473`)
   - Media Box (`mdia` / `0x6D646961`)
   - Media Information Box (`minf` / `0x6D696E66`)
   - Sample Table Box (`stbl` / `0x7374626C`)
2. Header and metadata leaf boxes:
   - Movie Header Box (`mvhd` / `0x6D766864`)
   - Track Header Box (`tkhd` / `0x746B6864`)
   - Edit List Box (`elst` / `0x656C7374`)
   - Media Header Box (`mdhd` / `0x6D646864`)
   - Handler Reference Box (`hdlr` / `0x68646C72`)
3. Support for FullBox `version == 0` (32-bit timestamp and duration fields) and `version == 1` (64-bit timestamp and duration fields), consuming exact wire bit spans per ISO/IEC 14496-12:2015.
   - For a `size == 1` box, the 64-bit `largesize` field is consumed immediately after `size` and `type`, before the FullBox payload. This applies to both containers and the dedicated header/metadata boxes below.
   - FullBox versions other than `0` and `1` are reported as `UnsupportedSyntax` at the version field; they must never fall back to the version-0 layout.
4. Clear boundaries:
   - Sample table paging (`stts`, `stsc`, `stsz`, `stco`, `co64`) is deferred to Task P5g.
   - Codec configuration boxes (`stsd`, `avc1`, `mp4a`, `avcC`, `esds`) are deferred to Task P5h.
   - In P5f, `stbl` payload boxes without dedicated headers are handled as standard boxes with `@lazy` payloads.

---

## Decision

### 1. Package Manifest (`src/rules/official/org.streamview.mp4/rule.toml`)

The package version is incremented from `0.1.0` to `0.1.1`:

```toml
manifest-version = 1

[package]
id = "org.streamview.mp4"
version = "0.1.1"
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

### 2. DSL Box Grammar and Container Recursion

All container boxes (`moov`, `trak`, `edts`, `mdia`, `minf`, `stbl`) use `@lazy(...) bytes ... @container(Box);` to re-enter `Mp4IsobmffAnalyzer` container scanning through `BoundedSourceView`.

```svfmt
@spec("ISO/IEC 14496-12:2015", "4.2")
@description("ISO Base Media File Format box.")
struct Box {
    bits<32> size
        @description("Box size in bytes. 1 indicates 64-bit largesize; 0 indicates extends to EOF.");
    bits<32> type
        @description("Box FourCC type identifier.");
    if (type == 0x66747970) {
        ...
    } else {
        if (type == 0x6D6F6F76) {
            if (size == 1) {
                bits<64> moov_largesize;
                computed<u64> moov_large_payload_bytes = moov_largesize - 16;
                @lazy(moov_large_payload_bytes) bytes moov_large_payload @container(Box);
            } else {
                if (size == 0) {
                    computed<u64> moov_eof_payload_bytes = available_bytes();
                    @lazy(moov_eof_payload_bytes) bytes moov_eof_payload @container(Box);
                } else {
                    computed<u64> moov_payload_bytes = size - 8;
                    @lazy(moov_payload_bytes) bytes moov_payload @container(Box);
                }
            }
        } else {
            if (type == 0x6D766864) {
                if (size == 1) {
                    bits<64> mvhd_largesize;
                }
                bits<8> mvhd_version
                    @description("Version of movie header box (0 or 1).");
                bits<24> mvhd_flags
                    @description("Flags (reserved).");
                if (mvhd_version == 1) {
                    bits<64> mvhd_v1_creation_time
                        @description("Creation time (seconds since 1904-01-01).");
                    bits<64> mvhd_v1_modification_time
                        @description("Modification time (seconds since 1904-01-01).");
                    bits<32> mvhd_v1_timescale
                        @description("Time scale units per second.");
                    bits<64> mvhd_v1_duration
                        @description("Duration of movie in timescale units.");
                } else {
                    if (mvhd_version == 0) {
                        bits<32> mvhd_v0_creation_time
                            @description("Creation time (seconds since 1904-01-01).");
                        bits<32> mvhd_v0_modification_time
                            @description("Modification time (seconds since 1904-01-01).");
                        bits<32> mvhd_v0_timescale
                            @description("Time scale units per second.");
                        bits<32> mvhd_v0_duration
                            @description("Duration of movie in timescale units.");
                    } else {
                        unsupported("Unsupported mvhd FullBox version") at mvhd_version;
                    }
                }
                bits<32> mvhd_rate
                    @description("Playback rate (fixed-point 16.16, 0x00010000 is 1.0).");
                bits<16> mvhd_volume
                    @description("Audio volume (fixed-point 8.8, 0x0100 is full).");
                bits<16> mvhd_reserved;
                bits<64> mvhd_reserved_2;
                computed<u64> mvhd_matrix_count = 9;
                repeat (mvhd_matrix_count, 9) {
                    bits<32> mvhd_matrix;
                }
                computed<u64> mvhd_pre_defined_count = 6;
                repeat (mvhd_pre_defined_count, 6) {
                    bits<32> mvhd_pre_defined;
                }
                bits<32> mvhd_next_track_id
                    @description("Next track ID to assign.");
            } else {
                ...
            }
        }
    }
}
```

### 3. Header Fields Contract

1. **Movie Header (`mvhd`, clause 8.2.2)**:
   - `mvhd_version`: `bits<8>`
   - `mvhd_flags`: `bits<24>`
   - `creation_time`, `modification_time`, `duration`: 64-bit if `version == 1`, 32-bit if `version == 0`
   - `timescale`: `bits<32>`
   - `rate`: `bits<32>` (16.16 fixed-point)
   - `volume`: `bits<16>` (8.8 fixed-point)
   - `reserved`: `bits<16>` + `bits<64>`
   - `matrix`: 9 elements of `bits<32>`
   - `pre_defined`: 6 elements of `bits<32>`
   - `next_track_id`: `bits<32>`

2. **Track Header (`tkhd`, clause 8.3.2)**:
   - `tkhd_version`: `bits<8>`
   - `tkhd_flags`: `bits<24>`
   - `creation_time`, `modification_time`, `duration`: 64-bit if `version == 1`, 32-bit if `version == 0`
   - `track_id`: `bits<32>`
   - `reserved`: `bits<32>`
   - `reserved_2`: `bits<64>`
   - `layer`: `bits<16>`
   - `alternate_group`: `bits<16>`
   - `volume`: `bits<16>`
   - `reserved_3`: `bits<16>`
   - `matrix`: 9 elements of `bits<32>`
   - `width`: `bits<32>` (16.16 fixed-point)
   - `height`: `bits<32>` (16.16 fixed-point)

3. **Media Header (`mdhd`, clause 8.4.2)**:
   - `mdhd_version`: `bits<8>`
   - `mdhd_flags`: `bits<24>`
   - `creation_time`, `modification_time`, `duration`: 64-bit if `version == 1`, 32-bit if `version == 0`
   - `timescale`: `bits<32>`
   - `pad`: `bits<1>`
   - `language`: `bits<15>` (packed 3x5-bit ISO-639-2/T)
   - `pre_defined`: `bits<16>`

4. **Handler Reference (`hdlr`, clause 8.4.3)**:
   - `hdlr_version`: `bits<8>`
   - `hdlr_flags`: `bits<24>`
   - `pre_defined`: `bits<32>`
   - `handler_type`: `bits<32>`
   - `reserved_0`, `reserved_1`, `reserved_2`: 3 elements of `bits<32>` (statically byte-aligned for following lazy byte region)
   - `name`: `@lazy` bytes

5. **Edit List (`elst`, clause 8.6.6)**:
   - `elst_version`: `bits<8>`
   - `elst_flags`: `bits<24>`
   - `elst_entry_count`: `bits<32>`
   - `repeat (elst_entry_count, 64)` (P5f accepts at most 64 entries; a larger count is `InvalidSyntax` to preserve the bounded DSL projection contract):
     - `segment_duration`, `media_time`: 64-bit if `version == 1`, 32-bit if `version == 0`
     - `media_rate_integer`: `bits<16>`
     - `media_rate_fraction`: `bits<16>`

---

## Consequences

- End-to-end MP4 analysis now parses full 5-level container hierarchies (`moov -> trak -> mdia -> minf -> stbl`) without hardcoded C++ logic.
- Time headers in version 0 and version 1 consume exact wire bits.
- Large-size `hdlr` consumes `largesize` before the FullBox payload and preserves the handler name span.
- Unsupported FullBox versions retain the decoded prefix and produce a warning without decoding a v0 suffix.
- P5g sample table indexing and P5h codec configuration remain cleanly isolated for subsequent tasks.
