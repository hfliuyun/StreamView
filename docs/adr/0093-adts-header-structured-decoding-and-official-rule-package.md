# ADR-0093: ADTS Header Structured Decoding and Official AAC Rule Package

## Status

Proposed

## Context

Task T15 established the ADTS frame enumeration capability (`AacAdtsScanner` / `DslScannerKind::AacAdtsFrame`), candidate format detection (`detectAacAdtsCandidate`), and application session polymorphic decoupling (`AnalysisSession`).

Task T16 introduces the official AAC rule package (`org.streamview.aac`, starting at version `0.1.0`), defines the formal ADTS header grammar in the DSL format language (`src/aac_adts.svfmt`), and activates end-to-end AAC stream analysis within StreamView.

According to ISO/IEC 14496-3:2019 (Edition 5), Audio Data Transport Stream (ADTS) frames comprise:
1. `adts_fixed_header` (28 bits, subclause 1.6.2.1, Table 1.6.2.1): Bitstream syncword, MPEG audio version, layer, CRC protection flag, profile, sampling frequency index, private bit, channel configuration, original/copy, and home.
2. `adts_variable_header` (28 bits, subclause 1.6.2.1, Table 1.6.2.1): Copyright identification bits, total frame length (`aac_frame_length`), buffer fullness (`adts_buffer_fullness`), and number of raw data blocks minus 1 (`number_of_raw_data_blocks_in_frame`).
3. `adts_error_check` (16 bits, subclause 1.6.2.2, Table 1.6.2.2): Present conditionally when `protection_absent == 0`.
4. `adts_raw_data_block` (subclause 1.6.2.3): Audio payload containing syntactical elements (SCE, CPE, LFE, DSE, PCE, FIL, TERM).

## Decision

We specify and implement the formal `org.streamview.aac` rule package and ADTS header structured decoding:

### 1. Official Package Manifest (`rule.toml`)

The AAC rule package is established at `src/rules/official/org.streamview.aac/rule.toml` with initial version `0.1.0`:

```toml
manifest-version = 1

[package]
id = "org.streamview.aac"
version = "0.1.0"
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
```

The package assets are bundled into the binary via Qt resources (`official_packages.qrc`) and resolved through `AacAdtsAnalyzer::create(source, errorMessage)` matching the `bundledH264AnnexBRule()` pattern from `h264_annex_b_analyzer.cpp`.

### 2. DSL Syntax Specification (`src/aac_adts.svfmt`)

The formal format definition is declared as follows:

```svfmt
enum AacProfile {
    main = 0;
    lc = 1;
    ssr = 2;
    ltp = 3;
}

enum AacChannelConfiguration {
    custom = 0;
    mono = 1;
    stereo = 2;
    three_channel = 3;
    four_channel = 4;
    five_channel = 5;
    five_one = 6;
    seven_one = 7;
}

@spec("ISO/IEC 14496-3:2019", "1.6.2.1")
@description("Audio Data Transport Stream (ADTS) fixed and variable header.")
struct AdtsHeader {
    bits<12> syncword @equals(4095)
        @description("12-bit syncword, always 0xFFF.");
    bits<1> id
        @description("MPEG audio version: 0 for MPEG-4, 1 for MPEG-2.");
    bits<2> layer @equals(0)
        @description("MPEG layer, always 0 for AAC.");
    bits<1> protection_absent
        @description("0 indicates 16-bit CRC is present; 1 indicates CRC absent.");
    bits<2> profile @enum(AacProfile)
        @description("AAC object type profile.");
    bits<4> sampling_frequency_index @range(0, 12)
        @description("Sampling frequency index from 0 (96 kHz) to 12 (7.35 kHz).");
    bits<1> private_bit
        @description("Private bit for application usage.");
    bits<3> channel_configuration @enum(AacChannelConfiguration)
        @description("Channel configuration.");
    bits<1> original_copy
        @description("Original/copy bit.");
    bits<1> home
        @description("Home/reserved bit.");
    bits<1> copyright_identification_bit
        @description("Copyright identification bit.");
    bits<1> copyright_identification_start
        @description("Copyright identification start.");
    bits<13> aac_frame_length
        @description("Total frame length in bytes including headers, CRC, and payload.");
    bits<11> adts_buffer_fullness
        @description("Buffer fullness; 0x7FF indicates variable bit rate (VBR).");
    bits<2> number_of_raw_data_blocks_in_frame @equals(0)
        @description("Number of raw data blocks minus 1; 0 denotes single raw data block.");
    if (protection_absent == 0) {
        bits<16> crc_check
            @spec("ISO/IEC 14496-3:2019", "1.6.2.2")
            @description("16-bit CRC error check word.");
    }
    computed<u64> minimum_frame_length =
        (protection_absent == 1) * 7 +
        (protection_absent == 0) * 9;
    assert(aac_frame_length >= minimum_frame_length) at aac_frame_length;
}

@index(progressive)
sequence<AdtsHeader> frames = scan(adts_frame);
entry frames;
```

### 3. Value Domain Classification & Diagnostic Strategy

Following the ADR-0040 dichotomy:

1. **Layout-Critical Invariants**:
   - `syncword @equals(4095)`: 12-bit `0xFFF`. Rejection terminates frame parsing and invokes resynchronization.
   - `layer @equals(0)`: In ISO/IEC 14496-3 / ISO/IEC 13818-7, `layer == 0` is strictly required for AAC.
   - `number_of_raw_data_blocks_in_frame @equals(0)`: Constrains parsing to streams with 1 raw data block per frame.
2. **Value-Domain Classifications**:
   - `sampling_frequency_index`: Constrained with `@range(0, 12)` (ISO/IEC 14496-3 Table 1.16). Values 13, 14, and 15 (escape value forbidden in ADTS per subclause 1.6.2.1) produce non-fatal diagnostic warnings without stopping frame decoding.
   - `profile`: Declared with 4-value enumeration `enum AacProfile` (`0` Main, `1` LC, `2` SSR, `3` LTP). Note: profile `3` (LTP) is reserved in MPEG-2 AAC (`id == 1`).
   - `channel_configuration`: Full 8-value enumeration `enum AacChannelConfiguration` (`0` Custom/PCE, `1` Mono, `2` Stereo, `3` 3-channel, `4` 4-channel, `5` 5-channel, `6` 5.1, `7` 7.1). `channel_configuration == 0` indicates a Program Config Element (PCE) in the raw data block (supported in Task T18).
   - `adts_buffer_fullness`: `0x7FF` is a valid normative indicator denoting variable bit rate (VBR) streams.
3. **Source-Anchored Minimum Frame Length Assertion**:
   - Guarded via `computed<u64> minimum_frame_length` using ADR-0090 boolean arithmetic.
   - Verified via `assert(aac_frame_length >= minimum_frame_length) at aac_frame_length;` to emit a source-anchored diagnostic if a corrupted frame declares a length smaller than the header byte count.

### 4. Explicit Postponement

1. Streams with `number_of_raw_data_blocks_in_frame > 0` (multiple raw data blocks with inter-block 16-bit CRC headers `raw_data_block_position`) are explicitly postponed.
2. Structured parsing of inner audio syntactical elements inside `raw_data_block` (ASC, SCE, CPE, LFE, PCE) is explicitly postponed to Tasks T17 and T18.

### 5. Architectural Probe Verification

The `.svfmt` rule definition was verified via `svtool rule check` against the StreamView rule compiler:

```bash
$ ./build/dev/tools/svtool/svtool rule check scratch/probe_t16_adts.svfmt
Rule OK: /Users/yun/.gemini/antigravity-cli/brain/12458dc0-7cd4-40c3-b0af-86d27dcb7b62/scratch/probe_t16_adts.svfmt
```

## References

- ISO/IEC 14496-3:2019, Edition 5, Subclauses 1.6.2.1, 1.6.2.2, 1.6.2.3, Tables 1.11, 1.16, 1.17, 1.6.2.1, 1.6.2.2
- ADR-0010: C-Style Declarative Format Description Language
- ADR-0016: TOML Manifest And ZIP Rule Packages
- ADR-0040: Report Unsigned Exp-Golomb Range Violations Without Stopping Decoding
- ADR-0054: Source-Anchored Assertion Statements
- ADR-0090: Boolean Operands In Arithmetic Expressions
- ADR-0092: AAC ADTS Frame Enumeration Mechanism and Official Rule Package
