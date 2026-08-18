# ADR-0102: MP4 Sample Descriptions and Codec Configuration Rules v0.1.3

- **Status**: Proposed
- **Date**: 2026-08-18
- **Authors**: StreamView Contributors

---

## Context

Task P5g completed the sample table indexing and window paging rules (`org.streamview.mp4` v0.1.2), providing windowed pagination for `stts`, `stsc`, `stsz`, `stco`, and `co64`.

Task P5h implements the sample description (`stsd`), sample entry (`avc1`, `mp4a`), and codec configuration (`avcC`, `esds`) rules in `org.streamview.mp4` v0.1.3:
1. Sample Description Box (`stsd` / `0x73747364`): FullBox container holding visual and audio sample entries.
2. Visual Sample Entry (`avc1` / `0x61766331`): ISO/IEC 14496-12 78-byte visual sample entry header, hosting child codec configuration boxes.
3. Audio Sample Entry (`mp4a` / `0x6D703461`): ISO/IEC 14496-14 28-byte audio sample entry header, hosting child descriptor boxes.
4. AVC Configuration Box (`avcC` / `0x61766343`): ISO/IEC 14496-15 `AVCDecoderConfigurationRecord`, exposing repeated sequence parameter sets (SPS), picture parameter sets (PPS), and the High-profile SPS extension set as `@lazy(...) bytes` regions annotated with `@target_format("video.h264.nal")`.
5. Elementary Stream Descriptor Box (`esds` / `0x65736473`): the supported AAC subset of the ISO/IEC 14496-14 FullBox descriptor, validating `ES_Descriptor` (tag 0x03), `DecoderConfigDescriptor` (tag 0x04), and `DecoderSpecificInfo` (tag 0x05), consuming the three optional ES fields selected by their flags, and supporting 1–4 byte variable length continuation (MSB 0x80). The `AudioSpecificConfig` payload is exposed as a `@lazy(...) bytes` region annotated with `@target_format("audio.aac.asc")`.

All structures must:
- Validate FullBox header versions (`stsd` version 0, `esds` version 0) and `avcC` `configurationVersion == 1`, reporting `unsupported(...)` diagnostics on unhandled versions;
- Support standard 32-bit box sizes, 64-bit `largesize` (`size == 1`), and `size == 0` EOF span;
- Validate descriptor tags, reserved AVC/AAC bits, and reject a fifth continuation length byte with an `unsupported(...)` diagnostic; descriptor payload lengths are decoded but remain within the enclosing box view rather than creating a new bounded descriptor view;
- Preserve DSL byte-alignment tracking across integer-byte lazy regions, variable bounded/sentinel repeats, and switch-arm joins without ad-hoc C++ decoder shortcuts;
- Maintain strict boundaries: Task P5h produces AST nodes and `@target_format` metadata without eagerly invoking sub-format analyzers or implementing cross-layer navigation (deferred to Task P5i).

---

## Decision

### 1. Package Manifest (`src/rules/official/org.streamview.mp4/rule.toml`)

The package version is incremented from `0.1.2` to `0.1.3`:

```toml
manifest-version = 1

[package]
id = "org.streamview.mp4"
version = "0.1.3"
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

### 2. DSL Sample Description and Codec Configuration Schemas

```svfmt
@spec("ISO/IEC 14496-12:2015", "8.5.2")
@description("Sample Description Box.")
struct SampleDescriptionBox {
    bits<8> version
        @description("Version (0).");
    bits<24> flags
        @description("Flags.");
    bits<32> entry_count
        @description("Number of sample entries.");
    if (version == 0) {
        computed<u64> stsd_entries_bytes = available_bytes();
        @lazy(stsd_entries_bytes) bytes entries
            @description("Sample description entries.")
            @container(Box);
    } else {
        unsupported("Unsupported stsd FullBox version") at version;
    }
}

@spec("ISO/IEC 14496-12:2015", "8.5.2.2")
@description("Visual Sample Entry (AVC1).")
struct VisualSampleEntry {
    bits<48> reserved_entry
        @description("Reserved 6 bytes.");
    bits<16> data_reference_index
        @description("Data reference index.");
    bits<16> pre_defined
        @description("Pre-defined (0).");
    bits<16> reserved_1
        @description("Reserved (0).");
    bits<32> pre_defined_1_0
        @description("Pre-defined 1/3 (0).");
    bits<32> pre_defined_1_1
        @description("Pre-defined 2/3 (0).");
    bits<32> pre_defined_1_2
        @description("Pre-defined 3/3 (0).");
    bits<16> width
        @description("Visual display width in pixels.");
    bits<16> height
        @description("Visual display height in pixels.");
    bits<32> horizresolution
        @description("Horizontal resolution (0x00480000 = 72 dpi).");
    bits<32> vertresolution
        @description("Vertical resolution (0x00480000 = 72 dpi).");
    bits<32> reserved_2
        @description("Reserved (0).");
    bits<16> frame_count
        @description("Frames per sample (usually 1).");
    bits<64> compressorname_0
        @description("Compressor name 1/4.");
    bits<64> compressorname_1
        @description("Compressor name 2/4.");
    bits<64> compressorname_2
        @description("Compressor name 3/4.");
    bits<64> compressorname_3
        @description("Compressor name 4/4.");
    bits<16> depth
        @description("Color depth (0x0018 = 24-bit).");
    bits<16> pre_defined_2
        @description("Pre-defined (-1).");
    computed<u64> avc1_children_bytes = available_bytes();
    @lazy(avc1_children_bytes) bytes child_boxes
        @description("Visual sample entry child boxes.")
        @container(Box);
}

@spec("ISO/IEC 14496-14:2020", "5.6.1")
@description("Audio Sample Entry (MP4A).")
struct AudioSampleEntry {
    bits<48> reserved_entry
        @description("Reserved 6 bytes.");
    bits<16> data_reference_index
        @description("Data reference index.");
    bits<64> reserved_1
        @description("Reserved 8 bytes (0).");
    bits<16> channelcount
        @description("Channel count.");
    bits<16> samplesize
        @description("Sample size in bits (16).");
    bits<16> pre_defined
        @description("Pre-defined (0).");
    bits<16> reserved_2
        @description("Reserved (0).");
    bits<32> samplerate
        @description("Sample rate (16.16 fixed-point).");
    computed<u64> mp4a_children_bytes = available_bytes();
    @lazy(mp4a_children_bytes) bytes child_boxes
        @description("Audio sample entry child boxes.")
        @container(Box);
}

@spec("ISO/IEC 14496-15:2019", "5.2.4.1")
@description("AVC Decoder Configuration Box (avcC).")
struct AvcConfigurationBox {
    bits<8> configurationVersion
        @description("Configuration version (1).");
    if (configurationVersion == 1) {
        bits<8> avcProfileIndication
            @description("AVC profile indication.");
        bits<8> profile_compatibility
            @description("Profile compatibility flags.");
        bits<8> avcLevelIndication
            @description("AVC level indication.");
        bits<6> reserved_6bits @equals(63)
            @description("Reserved 6 bits (111111b).");
        bits<2> lengthSizeMinusOne
            @description("NAL unit length field size minus one.");
        bits<3> reserved_3bits @equals(7)
            @description("Reserved 3 bits (111b).");
        bits<5> numOfSequenceParameterSets
            @description("Number of sequence parameter sets.");
        repeat (numOfSequenceParameterSets, 31) {
            bits<16> sequenceParameterSetLength
                @description("SPS NAL unit length in bytes.");
            @lazy(sequenceParameterSetLength) bytes sequenceParameterSetNALUnit
                @description("SPS NAL unit bytes.")
                @target_format("video.h264.nal");
        }
        bits<8> numOfPictureParameterSets
            @description("Number of picture parameter sets.");
        repeat (numOfPictureParameterSets, 64) {
            bits<16> pictureParameterSetLength
                @description("PPS NAL unit length in bytes.");
            @lazy(pictureParameterSetLength) bytes pictureParameterSetNALUnit
                @description("PPS NAL unit bytes.")
                @target_format("video.h264.nal");
        }
        computed<bool> has_profile_extensions =
            avcProfileIndication == 100 || avcProfileIndication == 110 ||
            avcProfileIndication == 122 || avcProfileIndication == 144;
        if (has_profile_extensions) {
            bits<6> reserved_chroma_format @equals(63);
            bits<2> chroma_format;
            bits<5> reserved_bit_depth_luma @equals(31);
            bits<3> bit_depth_luma_minus8;
            bits<5> reserved_bit_depth_chroma @equals(31);
            bits<3> bit_depth_chroma_minus8;
            bits<8> numOfSequenceParameterSetExt;
            repeat (numOfSequenceParameterSetExt, 255) {
                bits<16> sequenceParameterSetExtLength;
                @lazy(sequenceParameterSetExtLength) bytes sequenceParameterSetExtNALUnit
                    @target_format("video.h264.nal");
            }
        }
    } else {
        unsupported("Unsupported avcC configurationVersion") at configurationVersion;
    }
}

@spec("ISO/IEC 14496-14:2020", "5.6.1")
@description("Elementary Stream Descriptor Box (esds).")
struct ElementaryStreamDescriptorBox {
    bits<8> version
        @description("Version (0).");
    bits<24> flags
        @description("Flags.");
    if (version == 0) {
        bits<8> es_tag @equals(3)
            @description("ES_DescrTag (0x03).");
        bits<1> es_len_more0;
        bits<7> es_len_val0;
        if (es_len_more0 == 1) {
            bits<1> es_len_more1;
            bits<7> es_len_val1;
            if (es_len_more1 == 1) {
                bits<1> es_len_more2;
                bits<7> es_len_val2;
                if (es_len_more2 == 1) {
                    bits<1> es_len_more3;
                    bits<7> es_len_val3;
                    if (es_len_more3 == 1) {
                        unsupported("ES_Descriptor length exceeds four bytes") at es_len_more3;
                    }
                }
            }
        }
        bits<16> es_id
            @description("ES ID.");
        bits<1> streamDependenceFlag
            @description("Stream dependence flag.");
        bits<1> urlFlag
            @description("URL flag.");
        bits<1> ocrStreamFlag
            @description("OCR stream flag.");
        bits<5> streamPriority
            @description("Stream priority.");
        if (streamDependenceFlag == 1) {
            bits<16> dependsOn_ES_ID;
        }
        if (urlFlag == 1) {
            bits<8> URLlength;
            @lazy(URLlength) bytes URLstring;
        }
        if (ocrStreamFlag == 1) {
            bits<16> OCR_ES_Id;
        }
        bits<8> dc_tag @equals(4)
            @description("DecoderConfigDescrTag (0x04).");
        bits<1> dc_len_more0;
        bits<7> dc_len_val0;
        if (dc_len_more0 == 1) {
            bits<1> dc_len_more1;
            bits<7> dc_len_val1;
            if (dc_len_more1 == 1) {
                bits<1> dc_len_more2;
                bits<7> dc_len_val2;
                if (dc_len_more2 == 1) {
                    bits<1> dc_len_more3;
                    bits<7> dc_len_val3;
                    if (dc_len_more3 == 1) {
                        unsupported("DecoderConfigDescriptor length exceeds four bytes") at dc_len_more3;
                    }
                }
            }
        }
        bits<8> objectTypeIndication @equals(64)
            @description("Object type indication (0x40 for Audio ISO/IEC 14496-3 AAC).");
        bits<6> streamType @equals(5)
            @description("Stream type (0x05 for AudioStream).");
        bits<1> upStream
            @description("Upstream flag.");
        bits<1> reserved_1bit @equals(1)
            @description("Reserved bit (1).");
        bits<24> bufferSizeDB
            @description("Buffer size in bytes.");
        bits<32> maxBitrate
            @description("Maximum bitrate.");
        bits<32> avgBitrate
            @description("Average bitrate.");
        bits<8> dsi_tag @equals(5)
            @description("DecSpecificInfoTag (0x05).");
        bits<1> dsi_len_more0;
        bits<7> dsi_len_val0;
        if (dsi_len_more0 == 1) {
            bits<1> dsi_len_more1;
            bits<7> dsi_len_val1;
            if (dsi_len_more1 == 1) {
                bits<1> dsi_len_more2;
                bits<7> dsi_len_val2;
                if (dsi_len_more2 == 1) {
                    bits<1> dsi_len_more3;
                    bits<7> dsi_len_val3;
                    if (dsi_len_more3 == 1) {
                        unsupported("DecoderSpecificInfo length exceeds four bytes") at dsi_len_more3;
                    } else {
                        computed<u64> asc_len4 = dsi_len_val0 * 2097152 + dsi_len_val1 * 16384 + dsi_len_val2 * 128 + dsi_len_val3;
                        @lazy(asc_len4) bytes asc_bytes4
                            @description("AudioSpecificConfig payload.")
                            @target_format("audio.aac.asc");
                    }
                } else {
                    computed<u64> asc_len3 = dsi_len_val0 * 16384 + dsi_len_val1 * 128 + dsi_len_val2;
                    @lazy(asc_len3) bytes asc_bytes3
                        @description("AudioSpecificConfig payload.")
                        @target_format("audio.aac.asc");
                }
            } else {
                computed<u64> asc_len2 = dsi_len_val0 * 128 + dsi_len_val1;
                @lazy(asc_len2) bytes asc_bytes2
                    @description("AudioSpecificConfig payload.")
                    @target_format("audio.aac.asc");
            }
        } else {
            computed<u64> asc_len1 = dsi_len_val0;
            @lazy(asc_len1) bytes asc_bytes1
                @description("AudioSpecificConfig payload.")
                @target_format("audio.aac.asc");
        }
    } else {
        unsupported("Unsupported esds FullBox version") at version;
    }
}
```

### 3. Top-level Box Dispatch

The top-level `Box` structure dispatches:
- `0x73747364` (`stsd`): `@container(SampleDescriptionBox)`
- `0x61766331` (`avc1`): `@container(VisualSampleEntry)`
- `0x6D703461` (`mp4a`): `@container(AudioSampleEntry)`
- `0x61766343` (`avcC`): `@container(AvcConfigurationBox)`
- `0x65736473` (`esds`): `@container(ElementaryStreamDescriptorBox)`

### 4. DSL Byte-Alignment Tracking Across Lazy Regions and Repeats

Because `@lazy(byte_count) bytes` consumes an integer number of bytes (`byte_count * 8` bits), a lazy region beginning at a byte boundary also ends at a byte boundary. The DSL compiler and IR lowering preserve `byteAligned = true` across lazy regions and repeat loop iterations when all constituent elements are byte-aligned.

---

## Consequences

- The declared P5h sample-description and AAC/AVC codec-configuration subset is decoded through declarative DSL rules; descriptor lengths remain bounded by the enclosing Box view.
- Codec configuration payloads attach `@target_format("video.h264.nal")` and `@target_format("audio.aac.asc")` metadata for downstream session navigation.
- Multi-byte descriptor length continuation, ES optional fields, High-profile SPS extensions, and repeated SPS/PPS structures are correctly modeled; a fifth continuation byte is reported as unsupported.
- Cross-layer analyzer resolution and UI navigation remain cleanly decoupled for Task P5i.
