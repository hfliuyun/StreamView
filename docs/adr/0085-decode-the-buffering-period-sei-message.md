# Decode the Buffering Period SEI Message

Status: Accepted
Date: 2026-08-15

## Context

ITU-T H.264 clause 7.3.2.3 defines the Supplemental Enhancement Information (SEI)
RBSP container, and clause 7.3.2.3.1 defines the payload syntax dispatch. Clause
D.1.1 and clause D.2.1 define the Buffering Period SEI message (`payloadType == 0`),
which specifies initial CPB removal delay and offset parameters for hypothetical
reference decoder (HRD) operations:

```text
buffering_period( payloadSize ) {
    seq_parameter_set_id                                        ue(v)
    if( NalHrdBpPresentFlag ) {
        for( SchedSelIdx = 0; SchedSelIdx <= cpb_cnt_minus1; SchedSelIdx++ ) {
            initial_cpb_removal_delay[ SchedSelIdx ]            u(v)
            initial_cpb_removal_delay_offset[ SchedSelIdx ]     u(v)
        }
    }
    if( VclHrdBpPresentFlag ) {
        for( SchedSelIdx = 0; SchedSelIdx <= cpb_cnt_minus1; SchedSelIdx++ ) {
            initial_cpb_removal_delay[ SchedSelIdx ]            u(v)
            initial_cpb_removal_delay_offset[ SchedSelIdx ]     u(v)
        }
    }
}
```

In clause D.2.1:
- `seq_parameter_set_id`: specifies the sequence parameter set that contains the sequence HRD attributes. Conformant values are `0..31`.
- `NalHrdBpPresentFlag`: flag indicating whether NAL HRD parameters are present in the active SPS (`nal_hrd_parameters_present_flag == 1`).
- `VclHrdBpPresentFlag`: flag indicating whether VCL HRD parameters are present in the active SPS (`vcl_hrd_parameters_present_flag == 1`).
- `initial_cpb_removal_delay[SchedSelIdx]`: bit field of width `initial_cpb_removal_delay_length_minus1 + 1` specifying the initial CPB arrival delay. When absent in SPS HRD parameters, `initial_cpb_removal_delay_length_minus1` defaults to 23 (24 bits).
- `initial_cpb_removal_delay_offset[SchedSelIdx]`: bit field of width `initial_cpb_removal_delay_length_minus1 + 1` specifying the initial CPB arrival delay offset.
- CPB count iterations: `cpb_cnt_minus1 + 1` schedules, bounded between 1 and 32 per ITU-T H.264 clause E.2.2.

Per ITU-T H.264 clause 7.3.2.3.1, if the decoded buffering period syntax elements do not align to a byte boundary, payload alignment bits (`bit_equal_to_one` followed by `bit_equal_to_zero` bits) align the message to the byte boundary. In the StreamView DSL, this is represented by `rbsp_trailing_bits;` inside `case 0:`.

### Context Export and Import Contracts

To decode `buffering_period` SEI payloads according to the referenced SPS:
1. `SequenceParameterSetRbsp` exports HRD scalar values:
   - `effective_nal_hrd_parameters_present_flag`: `optional_value(nal_hrd_parameters_present_flag, 0)`
   - `effective_nal_hrd_cpb_count`: `optional_value(nal_hrd_cpb_cnt_minus1, 0) + 1`
   - `effective_nal_hrd_initial_cpb_removal_delay_length_minus1`: `optional_value(nal_hrd_initial_cpb_removal_delay_length_minus1, 23)`
   - `effective_vcl_hrd_parameters_present_flag`: `optional_value(vcl_hrd_parameters_present_flag, 0)`
   - `effective_vcl_hrd_cpb_count`: `optional_value(vcl_hrd_cpb_cnt_minus1, 0) + 1`
   - `effective_vcl_hrd_initial_cpb_removal_delay_length_minus1`: `optional_value(vcl_hrd_initial_cpb_removal_delay_length_minus1, 23)`
2. `SeiRbsp` imports `h264-sps` using locally scoped context import key `seq_parameter_set_id` declared inside `case 0:` per ADR-0084.
3. Within `SeiRbsp`, NAL and VCL HRD fields are distinguished by prefix (`nal_initial_cpb_removal_delay`, `nal_initial_cpb_removal_delay_offset`, `vcl_initial_cpb_removal_delay`, `vcl_initial_cpb_removal_delay_offset`) to ensure unique field identifiers in the struct namespace.

## Decision

1. **SPS HRD Context Exports**:
   In `SequenceParameterSetRbsp`, export effective HRD scalar attributes using `optional_value` fallback semantics:
   ```svfmt
   computed<u64> effective_nal_hrd_parameters_present_flag =
       optional_value(nal_hrd_parameters_present_flag, 0) @context_export;
   computed<u64> effective_nal_hrd_cpb_count =
       optional_value(nal_hrd_cpb_cnt_minus1, 0) + 1 @context_export;
   computed<u64> effective_nal_hrd_initial_cpb_removal_delay_length_minus1 =
       optional_value(nal_hrd_initial_cpb_removal_delay_length_minus1, 23) @context_export;

   computed<u64> effective_vcl_hrd_parameters_present_flag =
       optional_value(vcl_hrd_parameters_present_flag, 0) @context_export;
   computed<u64> effective_vcl_hrd_cpb_count =
       optional_value(vcl_hrd_cpb_cnt_minus1, 0) + 1 @context_export;
   computed<u64> effective_vcl_hrd_initial_cpb_removal_delay_length_minus1 =
       optional_value(vcl_hrd_initial_cpb_removal_delay_length_minus1, 23) @context_export;
   ```

2. **Buffering Period SEI Decoding**:
   In `SeiRbsp`, when `payload_type == 0`, decode:
   ```svfmt
   case 0: {
       ue seq_parameter_set_id @range(0, 31)
           @spec("ITU-T H.264", "D.1.1, D.2.1")
           @description("Identifies the sequence parameter set containing the HRD parameters.");
       computed<u64> nal_hrd_bp_present =
           context_value(seq_parameter_set_id, h264_sps, effective_nal_hrd_parameters_present_flag);
       if (nal_hrd_bp_present == 1) {
           computed<u64> nal_cpb_count =
               context_value(seq_parameter_set_id, h264_sps, effective_nal_hrd_cpb_count);
           computed<u64> nal_delay_length =
               context_value(seq_parameter_set_id, h264_sps, effective_nal_hrd_initial_cpb_removal_delay_length_minus1) + 1;
           repeat (nal_cpb_count, 32) {
               bits<nal_delay_length> nal_initial_cpb_removal_delay
                   @spec("ITU-T H.264", "D.1.1, D.2.1")
                   @description("Specifies the default initial arrival delay for the NAL HRD CPB.");
               bits<nal_delay_length> nal_initial_cpb_removal_delay_offset
                   @spec("ITU-T H.264", "D.1.1, D.2.1")
                   @description("Specifies the initial arrival delay offset for the NAL HRD CPB.");
           }
       }
       computed<u64> vcl_hrd_bp_present =
           context_value(seq_parameter_set_id, h264_sps, effective_vcl_hrd_parameters_present_flag);
       if (vcl_hrd_bp_present == 1) {
           computed<u64> vcl_cpb_count =
               context_value(seq_parameter_set_id, h264_sps, effective_vcl_hrd_cpb_count);
           computed<u64> vcl_delay_length =
               context_value(seq_parameter_set_id, h264_sps, effective_vcl_hrd_initial_cpb_removal_delay_length_minus1) + 1;
           repeat (vcl_cpb_count, 32) {
               bits<vcl_delay_length> vcl_initial_cpb_removal_delay
                   @spec("ITU-T H.264", "D.1.1, D.2.1")
                   @description("Specifies the default initial arrival delay for the VCL HRD CPB.");
               bits<vcl_delay_length> vcl_initial_cpb_removal_delay_offset
                   @spec("ITU-T H.264", "D.1.1, D.2.1")
                   @description("Specifies the initial arrival delay offset for the VCL HRD CPB.");
           }
       }
       rbsp_trailing_bits;
   }
   ```

3. **Package Version**:
   Upgrade `org.streamview.h264` package version from `0.1.34` to `0.1.35`.

## Consequences

- Buffering period SEI messages are decoded according to the referenced SPS HRD configuration.
- SPS configurations with NAL HRD, VCL HRD, or both are properly dispatched and unrolled.
- When neither NAL HRD nor VCL HRD is present in SPS, only `seq_parameter_set_id` and payload trailing bits are decoded.
- When the referenced SPS has not been decoded or is unavailable, the import key fails gracefully with `DependencyUnavailable`, and the SEI container safely skips the message and proceeds to subsequent SEI messages in the same RBSP.

## Follow-up

- ADR-0043: Add Bounded H.264 HRD Parameters
- ADR-0080: Iterate Bounded Repeats Over RBSP Data with While-Repeat
- ADR-0081: Decode the Recovery Point SEI Message
- ADR-0084: Locally Scoped Context Import Keys
