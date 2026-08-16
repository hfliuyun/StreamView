# ADR-0094: AudioSpecificConfig and Program Config Element Structured Decoding

## Status
Proposed

## Context
In MPEG-4 Audio (ISO/IEC 14496-3:2019, Edition 5), decoder configuration for Advanced Audio Coding (AAC) streams is formally standardized via `AudioSpecificConfig` (ASC, subclause 1.6.2.1). While ADTS streams convey basic stream properties (profile, sample rate index, channel configuration) in every frame header (ADR-0092, ADR-0093), modern audio delivery mechanisms—including MPEG-4 Part 14 containers (MP4 `esds` box, subclause 1.6.6) and streaming protocols—rely on out-of-band `AudioSpecificConfig` to initialize the audio decoder before parsing compressed raw data blocks.

To fulfill Phase 4 of the StreamView implementation plan, the official `org.streamview.aac` rule package must provide structured decoding for:
1. **AudioSpecificConfig (ASC)**: Including base and extended Audio Object Types (`audioObjectType`), standard and explicit sampling frequencies (`samplingFrequencyIndex` / `samplingFrequency`), channel configurations, and General Audio configuration (`GASpecificConfig`);
2. **Program Config Element (PCE)**: For custom multichannel layouts when `channelConfiguration == 0` (subclause 1.6.2.1, Table 1.18);
3. **Container Context Export**: Enabling downstream container formats (such as ISO BMFF / MP4 in Stage 5) to resolve and bind the active `AudioSpecificConfig` definition via `streamview::core::ContextDirectory` using `ContextDefinitionKind::AacAudioSpecificConfig`.

### Probing and Language Capability Analysis
Prior to specifying the grammar, empirical probing was conducted on scratch fixtures using `svtool rule check` to verify DSL language boundary constraints:

1. **Sub-Structure Instantiation and Scope Isolation**:
   Attempting to declare nested struct instances inside a structure (e.g. `GASpecificConfig ga_specific_config;` or `ProgramConfigElement pce;`) fails at parser type checking:
   ```
   error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type
   ```
   Furthermore, referencing outer structure fields from separate sub-structures is blocked by compiler dominance analysis:
   ```
   error: Computed dependency is not guaranteed on the current branch
   ```
   Consequently, following the established and proven pattern of H.264 VUI/HRD (`SequenceParameterSetRbsp`), Slice Header (`IdrSliceLayerWithoutPartitioningRbsp`), and SEI messages (`SeiRbsp`), `GASpecificConfig` and conditional `ProgramConfigElement` syntax must be cleanly inlined and flattened within `AudioSpecificConfig`.

2. **PCE Variable-Length Arrays and Bounded Repeat**:
   `ProgramConfigElement` contains 6 variable-length channel element arrays (`front`, `side`, `back`, `lfe`, `assoc_data`, `valid_cc`) and a comment data byte sequence. Probing confirmed that the DSL bounded repeat syntax:
   ```svfmt
   repeat (num_front_channel_elements, 15) {
       bits<1> front_element_is_cpe;
       bits<4> front_element_tag_select;
   }
   ```
   accurately parses, lowers to IR, and validates for all element counts with their standard ISO/IEC 14496-3 maximum upper bounds (15, 15, 15, 3, 7, 15, and 255).

3. **PCE Byte Alignment Expression**:
   Attempting to use `align(8);` fails syntax parsing:
   ```
   error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type
   ```
   Attempting `repeat (7) while (!byte_aligned())` fails while condition parsing:
   ```
   error: Expected 'more_rbsp_data' in while condition
   ```
   Attempting ternary expressions `(cond ? val1 : val2)` fails lexing:
   ```
   error: Invalid character in DSL source
   ```
   However, utilizing ADR-0090 boolean arithmetic and integer modulo expressions:
   ```svfmt
   computed<u64> pce_total_bits = 31
       + mono_mixdown_present * 4
       + stereo_mixdown_present * 4
       + matrix_mixdown_idx_present * 3
       + num_front_channel_elements * 5
       + num_side_channel_elements * 5
       + num_back_channel_elements * 5
       + num_lfe_channel_elements * 4
       + num_assoc_data_elements * 4
       + num_valid_cc_elements * 5;

   computed<u64> pce_rem = pce_total_bits % 8;
   computed<u64> pce_alignment_bits = (8 - pce_rem) % 8;

   repeat (pce_alignment_bits, 7) {
       bits<1> byte_alignment_zero_bit @equals(0);
   }
   ```
   evaluates the exact bit remainder and validates 0 to 7 alignment zero bits with `Rule OK` in `svtool rule check`.

## Decision

### 1. Official Package Entry Point and Manifest
We register a dedicated entry point `asc` in `src/rules/official/org.streamview.aac/rule.toml` alongside `adts`:
```toml
manifest-version = 1

[package]
id = "org.streamview.aac"
version = "0.1.1"
authors = ["StreamView Contributors"]
license = "Apache-2.0"
dependencies = []

[compatibility]
language = "0.1"
engine = ">=0.1.0 <0.2.0"

[[entrypoints]]
id = "adts"
format = "audio.aac.adts"
source = "src/aac_adts.svfmt"
profiles = ["aac-adts"]
depth = "adts-frame"

[[entrypoints]]
id = "asc"
format = "audio.aac.asc"
source = "src/aac_asc.svfmt"
profiles = ["aac-asc"]
depth = "structural"
```

### 2. AudioSpecificConfig DSL Syntax Specification
We specify `src/rules/official/org.streamview.aac/src/aac_asc.svfmt` with standard ISO/IEC 14496-3:2019 subclause 1.6.2.1 syntax:

```svfmt
struct AudioSpecificConfig {
    bits<5> audio_object_type
        @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
        @description("Audio Object Type identifier.");
    if (audio_object_type == 31) {
        bits<6> audio_object_type_ext
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Audio Object Type extension for escape values.");
    }

    bits<4> sampling_frequency_index
        @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
        @range(0, 12)
        @description("Sampling frequency index.");
    if (sampling_frequency_index == 15) {
        bits<24> sampling_frequency
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Explicit sampling frequency in Hz.");
    }

    bits<4> channel_configuration
        @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
        @range(0, 7)
        @description("Channel configuration index (0 indicates custom PCE).");

    // GASpecificConfig syntax
    bits<1> frame_length_flag
        @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
        @description("Frame length flag (0: 1024 lines IMDCT, 1: 960 lines IMDCT).");
    bits<1> depends_on_core_coder
        @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
        @description("Core coder dependency flag.");
    if (depends_on_core_coder == 1) {
        bits<14> core_coder_delay
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Core coder delay value in samples.");
    }
    bits<1> extension_flag
        @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
        @description("Extension flag for audio object type specific extensions.");

    if (channel_configuration == 0) {
        // Program Config Element (PCE)
        bits<4> element_instance_tag
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("PCE instance identifier tag.");
        bits<2> object_type
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("PCE audio object type.");
        bits<4> pce_sampling_frequency_index
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @range(0, 12)
            @description("PCE sampling frequency index.");
        bits<4> num_front_channel_elements
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Number of front channel elements.");
        bits<4> num_side_channel_elements
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Number of side channel elements.");
        bits<4> num_back_channel_elements
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Number of back channel elements.");
        bits<2> num_lfe_channel_elements
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Number of low-frequency enhancement channel elements.");
        bits<3> num_assoc_data_elements
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Number of associated data elements.");
        bits<4> num_valid_cc_elements
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Number of valid coupling channel elements.");

        bits<1> mono_mixdown_present
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Mono mixdown present flag.");
        if (mono_mixdown_present == 1) {
            bits<4> mono_mixdown_element_number
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Mono mixdown element number.");
        }

        bits<1> stereo_mixdown_present
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Stereo mixdown present flag.");
        if (stereo_mixdown_present == 1) {
            bits<4> stereo_mixdown_element_number
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Stereo mixdown element number.");
        }

        bits<1> matrix_mixdown_idx_present
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Matrix mixdown index present flag.");
        if (matrix_mixdown_idx_present == 1) {
            bits<2> matrix_mixdown_idx
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Matrix mixdown index.");
            bits<1> pseudo_surround_enable
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Pseudo surround enable flag.");
        }

        repeat (num_front_channel_elements, 15) {
            bits<1> front_element_is_cpe
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Front channel element is channel pair element flag.");
            bits<4> front_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Front channel element instance tag selector.");
        }

        repeat (num_side_channel_elements, 15) {
            bits<1> side_element_is_cpe
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Side channel element is channel pair element flag.");
            bits<4> side_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Side channel element instance tag selector.");
        }

        repeat (num_back_channel_elements, 15) {
            bits<1> back_element_is_cpe
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Back channel element is channel pair element flag.");
            bits<4> back_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Back channel element instance tag selector.");
        }

        repeat (num_lfe_channel_elements, 3) {
            bits<4> lfe_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("LFE channel element instance tag selector.");
        }

        repeat (num_assoc_data_elements, 7) {
            bits<4> assoc_data_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Associated data element instance tag selector.");
        }

        repeat (num_valid_cc_elements, 15) {
            bits<1> cc_element_is_ind_sw
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Coupling channel element independently switched flag.");
            bits<4> valid_cc_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Valid coupling channel element instance tag selector.");
        }

        computed<u64> pce_total_bits = 31
            + mono_mixdown_present * 4
            + stereo_mixdown_present * 4
            + matrix_mixdown_idx_present * 3
            + num_front_channel_elements * 5
            + num_side_channel_elements * 5
            + num_back_channel_elements * 5
            + num_lfe_channel_elements * 4
            + num_assoc_data_elements * 4
            + num_valid_cc_elements * 5;

        computed<u64> pce_rem = pce_total_bits % 8;
        computed<u64> pce_alignment_bits = (8 - pce_rem) % 8;

        repeat (pce_alignment_bits, 7) {
            bits<1> byte_alignment_zero_bit
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @equals(0)
                @description("Byte alignment stuffing zero bit.");
        }

        bits<8> comment_field_bytes
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Length in bytes of comment field data.");
        repeat (comment_field_bytes, 255) {
            bits<8> comment_field_data
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Comment field data byte.");
        }
    }
}

entry AudioSpecificConfig;
```

### 3. Diagnostic and Truncation Semantics
- **Truncation (`DiagnosticCode::TruncatedSource`)**: If an ASC bitstream terminates prematurely (e.g. within `AudioSpecificConfig` header, `GASpecificConfig`, or inside PCE channel lists), `DslExecutor::decodeStruct` returns `DslExecutionStatus::TruncatedSource`. The partial structure is published with `DiagnosticCode::TruncatedSource` and `DiagnosticSeverity::Error` (`"Unable to read complete syntax field"`).
- **Alignment Error (`DiagnosticCode::InvalidSyntax`)**: If any PCE alignment stuffing bit is nonzero (`@equals(0)` fails), `DslExecutor` attaches `DiagnosticCode::InvalidSyntax` with `DiagnosticSeverity::Error` at the offending bit coordinate.
- **Value Domain Warnings (`DiagnosticCode::InvalidSyntax`)**: If `sampling_frequency_index` exceeds 12, or `channel_configuration` exceeds 7, the node is materialized with a `@range` warning diagnostic per ADR-0040.

## Consequences

### Positive
- Fully decodes standard AAC-LC `AudioSpecificConfig` bitstreams and custom multi-channel `ProgramConfigElement` layouts.
- Preserves single-pass linear streaming performance without overhead from sub-structure calls.
- Establishes a verified foundation for Stage 5 MP4 `esds` atom container parsing and context binding.
- Package version becomes `0.1.1`.

### Negative
- SBR/PS backward-compatible sync extension (`syncExtensionType == 0x2b7`) and non-GA audio object types are unparsed in this slice and deferred to dedicated extensions.

## References
- ISO/IEC 14496-3:2019, Information technology — Coding of audio-visual objects — Part 3: Audio (subclauses 1.6.2.1, 1.6.6).
- [ADR-0040: Non-Fatal Syntax Warnings and Range Annotations](0040-non-fatal-syntax-warnings-and-range-annotations.md)
- [ADR-0090: Boolean Arithmetic and Logical Expressions in DSL](0090-boolean-arithmetic-and-logical-expressions-in-dsl.md)
- [ADR-0092: AAC ADTS Frame Enumeration and Rule Package Architecture](0092-aac-adts-frame-enumeration-and-rule-package.md)
- [ADR-0093: ADTS Header Structured Decoding and Official Rule Package](0093-adts-header-structured-decoding-and-official-rule-package.md)
