# ADR-0091: H.264 Picture Timing SEI Message Decoding

## Status

Accepted

## Context

ITU-T H.264 clause D.1.2 / D.2.2 defines the Picture Timing SEI message (`payload_type == 1`), which provides HRD CPB removal and DPB output delays, picture structure (`pic_struct`), and clock timestamps for picture presentation and timing synchronization.

Decoding Picture Timing SEI messages requires resolving several syntactic and semantic dependencies:
1. **Active SPS Parameters**: Delay field bit widths (`cpb_removal_delay_length_minus1 + 1`, `dpb_output_delay_length_minus1 + 1`), `time_offset_length`, and `pic_struct_present_flag` originate from HRD and VUI parameters in the active Sequence Parameter Set (SPS). Because the SEI syntax does not carry an explicit SPS identifier, these parameters must be resolved from the latest active SPS via ambient context import (ADR-0086).
2. **HRD Parameter Hierarchy**: Per ITU-T H.264 clause E.1.2 / E.2.1 / D.2.2, when NAL HRD parameters are present, their length parameters govern. When NAL HRD is absent and VCL HRD is present, VCL HRD length parameters govern. When neither is present, default values specified in clause E.2.1 apply (23 for delay lengths, 24 for `time_offset_length`, 0 for `pic_struct_present_flag`).
3. **Table D-1 Picture Structure Mapping**: `pic_struct` maps to `NumClockTS` (number of clock timestamps) according to ITU-T H.264 Table D-1 ($0..2 \to 1$, $3, 4, 7 \to 2$, $5, 6, 8 \to 3$, $9..15 \to 0$).
4. **Conditional Payload Alignment**: The message payload may end on a byte boundary (e.g. exactly 48 bits of delays without `pic_struct`) or on an unaligned bit, requiring conditional trailing bit alignment via `byte_aligned()` (ADR-0089).

## Decision

We implement full ITU-T H.264 D.1.2 / D.2.2 Picture Timing SEI message decoding under package version `0.1.38`:

### 1. SPS Context Export Extensions
In `SequenceParameterSetRbsp` (`src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt`), we export 4 computed fields using nested `optional_value` expressions per clauses E.1.2 / E.2.1:
```svfmt
computed<u64> effective_cpb_removal_delay_length_minus1 =
    optional_value(nal_hrd_cpb_removal_delay_length_minus1,
                   optional_value(vcl_hrd_cpb_removal_delay_length_minus1, 23)) @context_export;
computed<u64> effective_dpb_output_delay_length_minus1 =
    optional_value(nal_hrd_dpb_output_delay_length_minus1,
                   optional_value(vcl_hrd_dpb_output_delay_length_minus1, 23)) @context_export;
computed<u64> effective_time_offset_length =
    optional_value(nal_hrd_time_offset_length,
                   optional_value(vcl_hrd_time_offset_length, 24)) @context_export;
computed<u64> effective_pic_struct_present_flag =
    optional_value(pic_struct_present_flag, 0) @context_export;
```

### 2. Ambient Context Import and Coexistence in `SeiRbsp`
`SeiRbsp` imports `@context_import("h264-sps")` (ambient context import) in coexistence with the existing keyed import `@context_import("h264-sps", seq_parameter_set_id)`. Per ADR-0086 §4:
- 2-argument `context_value(h264_sps, field)` binds to the ambient latest SPS;
- 3-argument `context_value(seq_parameter_set_id, h264_sps, field)` binds to the keyed SPS import.

### 3. CPB and DPB Delays Decoding
When `effective_nal_hrd_parameters_present_flag == 1 || effective_vcl_hrd_parameters_present_flag == 1`, `CpbDpbDelaysPresentFlag` is true, and the following fields are decoded:
- `bits<(context_value(h264_sps, effective_cpb_removal_delay_length_minus1) + 1)> cpb_removal_delay`
- `bits<(context_value(h264_sps, effective_dpb_output_delay_length_minus1) + 1)> dpb_output_delay`

### 4. Table D-1 Mapping via ADR-0090 Indicator Arithmetic
We define a top-level pure function using ADR-0090 boolean arithmetic:
```svfmt
pure u64 num_clock_ts_for_pic_struct(u64 pic_struct) {
    return (pic_struct <= 2) * 1 +
           (pic_struct == 3 || pic_struct == 4 || pic_struct == 7) * 2 +
           (pic_struct == 5 || pic_struct == 6 || pic_struct == 8) * 3;
}
```

### 5. Picture Structure and Clock Timestamp Decoding
When `context_value(h264_sps, effective_pic_struct_present_flag) == 1`:
- `bits<4> pic_struct @range(0, 8)`
- `computed<u64> num_clock_ts = num_clock_ts_for_pic_struct(pic_struct);`
- `repeat (num_clock_ts, 3)` expands up to 3 clock timestamps:
  - `bits<1> clock_timestamp_flag`
  - When `clock_timestamp_flag == 1`:
    - `bits<2> ct_type @range(0, 2)`
    - `bits<1> nuit_field_based_flag`
    - `bits<5> counting_type @range(0, 6)`
    - `bits<1> full_timestamp_flag`
    - `bits<1> discontinuity_flag`
    - `bits<1> cnt_dropped_flag`
    - `bits<8> n_frames`
    - In full timestamp mode (`full_timestamp_flag == 1`):
      - `bits<6> full_seconds_value @range(0, 59)`
      - `bits<6> full_minutes_value @range(0, 59)`
      - `bits<5> full_hours_value @range(0, 23)`
    - In partial timestamp mode (`full_timestamp_flag == 0`):
      - `bits<1> seconds_flag`
      - If `seconds_flag == 1`:
        - `bits<6> partial_seconds_value @range(0, 59)`
        - `bits<1> minutes_flag`
        - If `minutes_flag == 1`:
          - `bits<6> partial_minutes_value @range(0, 59)`
          - `bits<1> hours_flag`
          - If `hours_flag == 1`:
            - `bits<5> partial_hours_value @range(0, 23)`
    - Gated `time_offset` decoding:
      - `computed<bool> has_time_offset = context_value(h264_sps, effective_time_offset_length) > 0;`
      - `if (has_time_offset) { bits<(context_value(h264_sps, effective_time_offset_length))> time_offset; }`

### 6. Field Naming & Two's Complement Interpretation
- **Field Name Disambiguation**: In accordance with the DSL global field uniqueness requirement, timestamp fields in full and partial branches are prefixed with `full_` and `partial_` (mapping to standard clause D.1.2 `seconds_value`, `minutes_value`, `hours_value`).
- **`time_offset` Signedness**: Per Probe 1, `time_offset` is decoded as raw unsigned bits `bits<time_offset_length>`. The specification documentation clarifies that this value is interpreted as two's complement signed integer.

| Bundled Profile Field Name | ITU-T H.264 Clause D.1.2 Element Name | Condition / Branch | Range / Type |
|---|---|---|---|
| `cpb_removal_delay` | `cpb_removal_delay` | `CpbDpbDelaysPresentFlag == 1` | `bits<effective_cpb_removal_delay_length_minus1 + 1>` |
| `dpb_output_delay` | `dpb_output_delay` | `CpbDpbDelaysPresentFlag == 1` | `bits<effective_dpb_output_delay_length_minus1 + 1>` |
| `pic_struct` | `pic_struct` | `effective_pic_struct_present_flag == 1` | `bits<4> @range(0, 8)` |
| `num_clock_ts` | `NumClockTS` | Computed from Table D-1 | `computed<u64>` (0..3) |
| `clock_timestamp_flag` | `clock_timestamp_flag[i]` | Repeat iteration `i < num_clock_ts` | `bits<1>` |
| `ct_type` | `ct_type` | `clock_timestamp_flag == 1` | `bits<2> @range(0, 2)` |
| `nuit_field_based_flag` | `nuit_field_based_flag` | `clock_timestamp_flag == 1` | `bits<1>` |
| `counting_type` | `counting_type` | `clock_timestamp_flag == 1` | `bits<5> @range(0, 6)` |
| `full_timestamp_flag` | `full_timestamp_flag` | `clock_timestamp_flag == 1` | `bits<1>` |
| `discontinuity_flag` | `discontinuity_flag` | `clock_timestamp_flag == 1` | `bits<1>` |
| `cnt_dropped_flag` | `cnt_dropped_flag` | `clock_timestamp_flag == 1` | `bits<1>` |
| `n_frames` | `n_frames` | `clock_timestamp_flag == 1` | `bits<8>` |
| `full_seconds_value` | `seconds_value` | `full_timestamp_flag == 1` | `bits<6> @range(0, 59)` |
| `full_minutes_value` | `minutes_value` | `full_timestamp_flag == 1` | `bits<6> @range(0, 59)` |
| `full_hours_value` | `hours_value` | `full_timestamp_flag == 1` | `bits<5> @range(0, 23)` |
| `seconds_flag` | `seconds_flag` | `full_timestamp_flag == 0` | `bits<1>` |
| `partial_seconds_value` | `seconds_value` | `full_timestamp_flag == 0 && seconds_flag == 1` | `bits<6> @range(0, 59)` |
| `minutes_flag` | `minutes_flag` | `full_timestamp_flag == 0 && seconds_flag == 1` | `bits<1>` |
| `partial_minutes_value` | `minutes_value` | `full_timestamp_flag == 0 && seconds_flag == 1 && minutes_flag == 1` | `bits<6> @range(0, 59)` |
| `hours_flag` | `hours_flag` | `full_timestamp_flag == 0 && seconds_flag == 1 && minutes_flag == 1` | `bits<1>` |
| `partial_hours_value` | `hours_value` | `full_timestamp_flag == 0 && seconds_flag == 1 && minutes_flag == 1 && hours_flag == 1` | `bits<5> @range(0, 23)` |
| `time_offset` | `time_offset` | `clock_timestamp_flag == 1 && effective_time_offset_length > 0` | `bits<effective_time_offset_length>` (two's complement) |

### 7. Conditional Payload Alignment
Conditional byte alignment is applied at the end of the `case 1` message payload using ADR-0089:
```svfmt
if (!byte_aligned()) {
    rbsp_trailing_bits;
}
```

## Consequences

### Positive
- Completes structured decoding of ITU-T H.264 D.1.2 Picture Timing SEI messages.
- Exercises ambient SPS context resolution in production `.svfmt` rules.
- Fully supports both byte-aligned and non-aligned picture timing payloads without stream desynchronization.

### Package Version
- Upgrades `rule.toml` package version to `0.1.38`.

### Verification & Fixture Matrix
1. **Multi-SPS Ambient Binding**: Verifies that SEI correctly resolves the latest active SPS across multiple SPS definitions in the bitstream.
2. **Missing SPS Isolation**: Verifies that SEI without preceding SPS is marked with message-level `WaitingDependency` / `Invalid` diagnostic while subsequent SEI messages and AUD NALs continue parsing.
3. **No HRD, `pic_struct` Only Path**: Bitstream containing `pic_struct` without CPB/DPB delays.
4. **Byte-Aligned & Unaligned Payloads**: Bitstreams with 48-bit aligned payloads and unaligned payloads tested for precise node boundaries.
5. **Valid `pic_struct = 8`**: Valid frame tripling decoding with 3 clock timestamp structures.
6. **Out-of-Bounds `pic_struct = 9`**: Produces `@range` Warning while `NumClockTS = 0` skips timestamp parsing and preserves stream alignment.
7. **Zero `time_offset_length = 0`**: Verifies `time_offset` is omitted when length is 0.
8. **Truncated Payload**: Verifies truncated payload retains materialized prefix syntax fields.

## References

- [ADR-0023: Pure Function Inlining and Semantic Validation](0023-pure-function-inlining.md)
- [ADR-0086: Context Management for Ambient SEI and Layered Parameter Sets](0086-context-management-for-ambient-sei-and-layered-parameter-sets.md)
- [ADR-0089: `byte_aligned()` Source-Position Predicate Expression](0089-byte-aligned-predicate-expression.md)
- [ADR-0090: Boolean Operands in Additive and Multiplicative Arithmetic Expressions](0090-boolean-operands-in-arithmetic-expressions.md)
- ITU-T Recommendation H.264 (08/2021) Clause D.1.2 / D.2.2 & Table D-1
