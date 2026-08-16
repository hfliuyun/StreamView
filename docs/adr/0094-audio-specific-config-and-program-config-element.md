# ADR-0094: AudioSpecificConfig and Program Config Element Structured Decoding

## Status
Proposed

## Context
In MPEG-4 Audio (ISO/IEC 14496-3:2019, Edition 5), decoder configuration for Advanced Audio Coding (AAC) streams is formally standardized via `AudioSpecificConfig` (ASC, subclause 1.6.2.1). While ADTS streams convey basic stream properties (profile, sample rate index, channel configuration) in every frame header (ADR-0092, ADR-0093), modern audio delivery mechanisms—including MPEG-4 Systems descriptors (MP4 `esds` box per ISO/IEC 14496-1:2010 and ISO/IEC 14496-14) and streaming protocols—rely on `AudioSpecificConfig` to initialize the audio decoder before parsing compressed raw data blocks.

To fulfill Phase 4 of the StreamView implementation plan, the official `org.streamview.aac` rule package must provide structured decoding for:
1. **AudioSpecificConfig (ASC)**: Including base and extended Audio Object Types (`audioObjectType`), standard and explicit sampling frequencies (`samplingFrequencyIndex` / `samplingFrequency`), channel configurations, and General Audio configuration (`GASpecificConfig`, subclause 4.4.1);
2. **Program Config Element (PCE)**: For custom multichannel layouts when `channelConfiguration == 0` (subclause 4.4.1.1, Table 4.2);
3. **Multi-Entry Package Manifest**: Updating `org.streamview.aac` manifest (`rule.toml`) to version `0.1.2` with dedicated entry points for both `adts` and `asc`.

### Empirical Probing and Language Capability Analysis
Prior to specifying the grammar, empirical probing was conducted on scratch fixtures using `svtool rule check` to verify DSL language boundary constraints:

1. **Sub-Structure Instantiation and Scope Isolation**:
   Attempting to declare nested struct instances inside a structure (e.g. `GASpecificConfig ga_specific_config;` or `ProgramConfigElement pce;`) fails at parser type checking (`src/rules/dsl.cpp:1002`):
   ```
   error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type
   ```
   Furthermore, referencing outer structure fields from separate sub-structures is blocked by compiler dominance analysis (`src/rules/dsl.cpp:3394` and `src/rules/dsl_ir.cpp:1557-1561`):
   ```
   error: Computed dependency is not guaranteed on the current branch
   ```
   Consequently, following the established and proven pattern of H.264 VUI/HRD (`SequenceParameterSetRbsp`), Slice Header (`IdrSliceLayerWithoutPartitioningRbsp`), and SEI messages (`SeiRbsp`), `GASpecificConfig` and conditional `ProgramConfigElement` syntax must be cleanly inlined and flattened within `AudioSpecificConfig`.

2. **PCE Variable-Length Arrays and Bounded Repeat**:
   `ProgramConfigElement` contains 6 variable-length channel element arrays (`front`, `side`, `back`, `lfe`, `assoc_data`, `valid_cc`) and a comment data byte sequence. Probing confirmed that the DSL bounded repeat syntax (`src/rules/dsl.cpp:1265`):
   ```svfmt
   repeat (num_front_channel_elements, 15) {
       bits<1> front_element_is_cpe;
       bits<4> front_element_tag_select;
   }
   ```
   accurately parses, lowers to IR, and validates for all element counts with their standard ISO/IEC 14496-3 maximum upper bounds (15, 15, 15, 3, 7, 15, and 255).

3. **PCE Byte Alignment Expression and Exact Bit Accounting**:
   Attempting to use `align(8);` fails syntax parsing (`src/rules/dsl.cpp:1002`):
   ```
   error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type
   ```
   Attempting `repeat (7) while (!byte_aligned())` fails while condition parsing (`src/rules/dsl.cpp:1295-1298`):
   ```
   error: Expected 'more_rbsp_data' in while condition
   ```
   Attempting ternary expressions `(cond ? val1 : val2)` fails lexing (`src/rules/dsl.cpp:293`):
   ```
   error: Invalid character in DSL source
   ```
   To ensure byte alignment is exact across all bitstream variations without silent misalignment, bit accounting must encompass:
   - Base prefix before PCE: `audio_object_type` (5) + `sampling_frequency_index` (4) + `channel_configuration` (4) + `frame_length_flag` (1) + `depends_on_core_coder` (1) + `extension_flag` (1) = 16 bits;
   - Conditional prefix bits: `(audio_object_type == 31) * 6` + `(sampling_frequency_index == 15) * 24` + `depends_on_core_coder * 14`;
   - Fixed PCE header: `element_instance_tag` (4) + `object_type` (2) + `pce_sampling_frequency_index` (4) + `num_front_channel_elements` (4) + `num_side_channel_elements` (4) + `num_back_channel_elements` (4) + `num_lfe_channel_elements` (2) + `num_assoc_data_elements` (3) + `num_valid_cc_elements` (4) + `mono_mixdown_present` (1) + `stereo_mixdown_present` (1) + `matrix_mixdown_idx_present` (1) = 34 bits;
   - Variable PCE elements and mixdown payloads: `mono_mixdown_present * 4` + `stereo_mixdown_present * 4` + `matrix_mixdown_idx_present * 3` + `num_front_channel_elements * 5` + `num_side_channel_elements * 5` + `num_back_channel_elements * 5` + `num_lfe_channel_elements * 4` + `num_assoc_data_elements * 4` + `num_valid_cc_elements * 5`.

   Using ADR-0090 boolean arithmetic and integer modulo expressions:
   ```svfmt
   computed<u64> pce_total_bits = 16
       + (audio_object_type == 31) * 6
       + (sampling_frequency_index == 15) * 24
       + depends_on_core_coder * 14
       + 34
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
   By referencing only dominating guard fields (`audio_object_type`, `sampling_frequency_index`, `depends_on_core_coder`) rather than guarded fields, dominance analysis passes, and all alignment bits (0 to 7) are mathematically and empirically verified.

4. **Lifecycle and Entry Point Scope in Stage 4**:
   In Stage 4, `AacAdtsAnalyzer` is the primary streaming analyzer dispatched via automatic ADTS candidate detection. The newly added entry point `asc` in `rule.toml` serves as a dormant structural asset in Stage 4: it can be resolved via `RulePackageCatalog::resolve(identity, u"asc", ...)` and directly executed via `DslExecutor::decodeStruct` in unit tests. Full automatic dispatch for MP4 container `esds` atom context import occurs in Stage 5 (ADR-0028), and manual format selection in Stage 6.

## Decision

### 1. Official Package Entry Point and Manifest
We update `src/rules/official/org.streamview.aac/rule.toml` to version `0.1.2`, placing `detector = "aac-adts"` within the `adts` entry point table and preserving all published package metadata:
```toml
manifest-version = 1

[package]
id = "org.streamview.aac"
version = "0.1.2"
authors = ["StreamView contributors"]
license = "MIT"
dependencies = []

[compatibility]
language = "0.1"
engine = ">=0.1.0 <0.2.0"

[[entrypoints]]
id = "adts"
format = "audio.aac.adts"
source = "src/aac_adts.svfmt"
profiles = ["lc"]
depth = "adts-frame"
detector = "aac-adts"

[[entrypoints]]
id = "asc"
format = "audio.aac.asc"
source = "src/aac_asc.svfmt"
profiles = ["lc"]
depth = "structural"
```

### 2. AudioSpecificConfig DSL Syntax Specification
We specify `src/rules/official/org.streamview.aac/src/aac_asc.svfmt` covering syntax defined in ISO/IEC 14496-3:2019 subclauses 1.6.2.1 (`AudioSpecificConfig`), 4.4.1 (`GASpecificConfig`), and 4.4.1.1 (`program_config_element`):

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
        @description("Sampling frequency index (0..12 standard, 15 explicit).");
    if (sampling_frequency_index == 15) {
        bits<24> sampling_frequency
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Explicit sampling frequency in Hz.");
    }

    bits<4> channel_configuration
        @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
        @description("Channel configuration index (0 indicates custom PCE).");

    // GASpecificConfig syntax
    bits<1> frame_length_flag
        @spec("ISO/IEC 14496-3:2019", "4.4.1")
        @description("Frame length flag (0: 1024 lines IMDCT, 1: 960 lines IMDCT).");
    bits<1> depends_on_core_coder
        @spec("ISO/IEC 14496-3:2019", "4.4.1")
        @description("Core coder dependency flag.");
    if (depends_on_core_coder == 1) {
        bits<14> core_coder_delay
            @spec("ISO/IEC 14496-3:2019", "4.4.1")
            @description("Core coder delay value in samples.");
    }
    bits<1> extension_flag
        @spec("ISO/IEC 14496-3:2019", "4.4.1")
        @description("Extension flag for audio object type specific extensions.");

    if (channel_configuration == 0) {
        // Program Config Element (PCE)
        bits<4> element_instance_tag
            @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
            @description("PCE instance identifier tag.");
        bits<2> object_type
            @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
            @description("PCE audio object type.");
        bits<4> pce_sampling_frequency_index
            @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
            @range(0, 12)
            @description("PCE sampling frequency index (0..12 standard).");
        bits<4> num_front_channel_elements
            @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
            @description("Number of front channel elements.");
        bits<4> num_side_channel_elements
            @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
            @description("Number of side channel elements.");
        bits<4> num_back_channel_elements
            @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
            @description("Number of back channel elements.");
        bits<2> num_lfe_channel_elements
            @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
            @description("Number of low-frequency enhancement channel elements.");
        bits<3> num_assoc_data_elements
            @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
            @description("Number of associated data elements.");
        bits<4> num_valid_cc_elements
            @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
            @description("Number of valid coupling channel elements.");

        bits<1> mono_mixdown_present
            @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
            @description("Mono mixdown present flag.");
        if (mono_mixdown_present == 1) {
            bits<4> mono_mixdown_element_number
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("Mono mixdown element number.");
        }

        bits<1> stereo_mixdown_present
            @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
            @description("Stereo mixdown present flag.");
        if (stereo_mixdown_present == 1) {
            bits<4> stereo_mixdown_element_number
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("Stereo mixdown element number.");
        }

        bits<1> matrix_mixdown_idx_present
            @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
            @description("Matrix mixdown index present flag.");
        if (matrix_mixdown_idx_present == 1) {
            bits<2> matrix_mixdown_idx
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("Matrix mixdown index.");
            bits<1> pseudo_surround_enable
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("Pseudo surround enable flag.");
        }

        repeat (num_front_channel_elements, 15) {
            bits<1> front_element_is_cpe
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("Front channel element is channel pair element flag.");
            bits<4> front_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("Front channel element instance tag selector.");
        }

        repeat (num_side_channel_elements, 15) {
            bits<1> side_element_is_cpe
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("Side channel element is channel pair element flag.");
            bits<4> side_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("Side channel element instance tag selector.");
        }

        repeat (num_back_channel_elements, 15) {
            bits<1> back_element_is_cpe
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("Back channel element is channel pair element flag.");
            bits<4> back_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("Back channel element instance tag selector.");
        }

        repeat (num_lfe_channel_elements, 3) {
            bits<4> lfe_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("LFE channel element instance tag selector.");
        }

        repeat (num_assoc_data_elements, 7) {
            bits<4> assoc_data_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("Associated data element instance tag selector.");
        }

        repeat (num_valid_cc_elements, 15) {
            bits<1> cc_element_is_ind_sw
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("Coupling channel element independently switched flag.");
            bits<4> valid_cc_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("Valid coupling channel element instance tag selector.");
        }

        computed<u64> pce_total_bits = 16
            + (audio_object_type == 31) * 6
            + (sampling_frequency_index == 15) * 24
            + depends_on_core_coder * 14
            + 34
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
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @equals(0)
                @description("Byte alignment stuffing zero bit.");
        }

        bits<8> comment_field_bytes
            @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
            @description("Length in bytes of comment field data.");
        repeat (comment_field_bytes, 255) {
            bits<8> comment_field_data
                @spec("ISO/IEC 14496-3:2019", "4.4.1.1")
                @description("Comment field data byte.");
        }
    }
}

entry AudioSpecificConfig;
```

### 3. Diagnostic and Truncation Semantics
- **Truncation (`DiagnosticCode::TruncatedSource`)**: If an ASC bitstream terminates prematurely (e.g. within `AudioSpecificConfig` header, `GASpecificConfig`, or inside PCE channel lists), `DslExecutor::decodeStruct` returns `DslExecutionStatus::TruncatedSource`. The partial structure is published with `DiagnosticCode::TruncatedSource` and `DiagnosticSeverity::Error` (`"Unable to read complete syntax field"`).
- **Alignment Error (`DiagnosticCode::InvalidSyntax`)**: If any PCE alignment stuffing bit is nonzero (`@equals(0)` fails), `DslExecutor` attaches `DiagnosticCode::InvalidSyntax` with `DiagnosticSeverity::Error` at the offending bit coordinate.
- **Reserved Value Inexpressibility Boundary**: In ASC, `sampling_frequency_index` has valid set `{0..12, 15}` (where 13..14 are reserved), and `channel_configuration` has valid set `{0..7}` (where 8..15 are reserved). Because `{0..12, 15}` is non-contiguous, it cannot be bounded by a single `@range(min, max)`; furthermore, `@enum` produces terminal syntax errors (`src/rules/dsl_vm.cpp:2782-2799`), which would violate ADR-0040's mandate that reserved values emit non-fatal warnings rather than fatal parse failures. Consequently, reserved values in ASC pass without diagnostics in this slice, and this constraint is formally recorded as a DSL language expressibility limitation.
- **Non-GA Audio Object Types**: Bitstreams with non-GA `audio_object_type` values (e.g. SBR = 5) parse basic GA header syntax without failing, while specific extension payloads are deferred to dedicated profile additions.

## Verification Matrix and Evidence

Static validation executed via `svtool rule check` on scratch probe:
```
$ ./build/dev/tools/svtool/svtool rule check /Users/yun/.gemini/antigravity-cli/brain/12458dc0-7cd4-40c3-b0af-86d27dcb7b62/scratch/probe_asc_complete.svfmt
Rule OK: /Users/yun/.gemini/antigravity-cli/brain/12458dc0-7cd4-40c3-b0af-86d27dcb7b62/scratch/probe_asc_complete.svfmt
```

Manifest loader verification executed via `RulePackage::fromFiles`:
```
fromFiles succeeded=1
  id=org.streamview.aac version=0.1.2 license=MIT
  entry id=adts format=audio.aac.adts depth=adts-frame detector=aac-adts
  entry id=asc format=audio.aac.asc depth=structural detector=<none>
```

Unit test coverage required for Task T17c (using bitstreams assembled by generator scripts with ground-truth alignment bit logging):
1. **Baseline ASC (Case 1)**: `aot = 2`, `sfi = 3`, `dcc = 0`, PCE elements all 0 $\to$ exact 6 alignment zero bits, `comment_field_bytes` decoded at byte offset 7;
2. **Explicit Core Coder Delay (Case 2)**: `dcc = 1` with 14-bit `core_coder_delay` $\to$ exact 0 alignment zero bits, `comment_field_bytes` decoded at byte offset 8;
3. **Extended Audio Object Type (Case 3)**: `aot = 31` with 6-bit `aot_ext` $\to$ exact 0 alignment zero bits, `comment_field_bytes` decoded at byte offset 7;
4. **Explicit 24-bit Frequency (Case 4)**: `sfi = 15` with 24-bit `sampling_frequency` $\to$ exact 6 alignment zero bits, `comment_field_bytes` decoded at byte offset 10;
5. **Multichannel Front/LFE (Case 5)**: `num_front_channel_elements = 2` (10 bits) + `num_lfe_channel_elements = 1` (4 bits) $\to$ exact 0 alignment zero bits, `comment_field_bytes` decoded at byte offset 8;
6. **Mixdown Flags Set (Case 6)**: `mono_mixdown_present = 1`, `stereo_mixdown_present = 1`, `matrix_mixdown_idx_present = 1` $\to$ exact 3 alignment zero bits, `comment_field_bytes` decoded at byte offset 8;
7. **Multichannel Side/Back/Assoc/CC (Case 7)**: `num_side_channel_elements = 1`, `num_back_channel_elements = 1`, `num_assoc_data_elements = 1`, `num_valid_cc_elements = 1` $\to$ exact 3 alignment zero bits, `comment_field_bytes` decoded at byte offset 9;
8. **PCE Non-zero Alignment Bit Rejection**: Stream with nonzero stuffing bit $\to$ `materialized = 0` with `DiagnosticCode::InvalidSyntax` (`@equals(0)` violation);
9. **ASC Premature Truncation**: Stream cut short during header or PCE $\to$ `materialized = 0` with `DiagnosticCode::TruncatedSource`.

## Consequences

### Positive
- Fully and accurately decodes standard AAC-LC `AudioSpecificConfig` bitstreams and custom multi-channel `ProgramConfigElement` layouts.
- Preserves single-pass linear streaming performance without overhead from sub-structure calls.
- Establishes a verified foundation for Stage 5 MP4 `esds` atom container parsing and context binding.
- Package version becomes `0.1.2`.

### Negative
- SBR/PS backward-compatible sync extension (`syncExtensionType == 0x2b7`) and non-GA audio object types are unparsed in this slice and deferred to dedicated extensions.

## References
- ISO/IEC 14496-3:2019, Information technology — Coding of audio-visual objects — Part 3: Audio (subclauses 1.6.2.1, 4.4.1, 4.4.1.1).
- ISO/IEC 14496-1:2010, Information technology — Coding of audio-visual objects — Part 1: Systems (subclause 7.2.6.5, Elementary Stream Descriptor).
- [ADR-0040: Non-Fatal Syntax Warnings and Range Annotations](0040-non-fatal-syntax-warnings-and-range-annotations.md)
- [ADR-0090: Boolean Arithmetic and Logical Expressions in DSL](0090-boolean-arithmetic-and-logical-expressions-in-dsl.md)
- [ADR-0092: AAC ADTS Frame Enumeration and Rule Package Architecture](0092-aac-adts-frame-enumeration-and-rule-package.md)
- [ADR-0093: ADTS Header Structured Decoding and Official Rule Package](0093-adts-header-structured-decoding-and-official-rule-package.md)

## Amendment: Subclause Reference and Version Correction

Subsequent specification auditing (Tasks T17d and T17e) clarified the distinct subclause citations under ISO/IEC 14496-3:2019 (Edition 5):
1. **AudioSpecificConfig**: Subpart 1 subclause **1.6.2.1** (*AudioSpecificConfig*).
2. **GASpecificConfig**: Subpart 4 subclause **4.4.1** (*GASpecificConfig* / *General Audio specific configuration*).
3. **program_config_element** (PCE): Subpart 4 subclause **4.4.1.1** (*Program config element (PCE)*).
4. **Package Version Bump**: Due to `.svfmt` content hash changes resulting from `@spec` citation corrections, the official `org.streamview.aac` package version is advanced to `0.1.2`.

## Amendment: Non-GA Audio Object Type Unsupported Diagnostics

Section 3 previously noted that non-GA `audio_object_type` values parse basic GA header syntax without failing. Following the implementation of explicit `unsupported` statements (ADR-0098) and AAC profile boundary harmonization (ADR-0095 §5.2), non-GA `audio_object_type` values (e.g. SBR = 5, PS = 29) and escaped AOT configurations (`audio_object_type == 31`) now immediately stop decoding after their common prefix and emit `DiagnosticCode::UnsupportedSyntax` (Severity: Warning, `MaterializationState::Unsupported`), precisely marking unsupported profiles as invalid/unsupported content rather than parsing downstream GA fields.
