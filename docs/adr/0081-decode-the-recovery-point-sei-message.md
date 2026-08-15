# Decode the Recovery Point SEI Message

Status: Accepted
Date: 2026-08-15

## Context

ITU-T H.264 clause 7.3.2.3 specifies the Supplemental Enhancement Information (SEI)
RBSP container, and clause 7.3.2.3.1 defines the payload syntax dispatch. Clause
D.1.7 and clause D.2.7 define the recovery point SEI message (`payloadType == 6`),
which informs the decoder that clean random access / recovery can occur starting
from the current picture:

```text
recovery_point( payloadSize ) {
    recovery_frame_cnt                               ue(v)
    exact_match_flag                                 u(1)
    broken_link_flag                                 u(1)
    changing_slice_group_idc                         u(2)
}
```

Clause 7.3.2.3.1 specifies that SEI message payloads that are not byte-aligned
terminate with alignment bits:

```text
if( !byte_aligned( ) ) {
    bit_equal_to_one /* equal to 1 */
    while( !byte_aligned( ) )
        bit_equal_to_zero /* equal to 0 */
}
```

Because `recovery_frame_cnt` is encoded with unsigned Exp-Golomb (`ue(v)`), its bit
width $W_{ue} = 2k + 1$ is always odd. With the subsequent 4 bits (`exact_match_flag`,
`broken_link_flag`, and `changing_slice_group_idc`), the payload bit length is
$2k + 5$, which is never a multiple of 8. Therefore, a recovery point SEI message
payload always contains trailing alignment bits (`bit_equal_to_one` followed by
0..6 `bit_equal_to_zero` bits) to reach the next byte boundary.

### Probe Results and DSL Capability Analysis

Probing the recovery point structure in scratch with `svtool rule check` revealed that
the DSL compiler previously rejected `rbsp_trailing_bits;` inside conditional branches:
```text
probe_sei_recovery_point.svfmt:15:17: error: rbsp_trailing_bits must occur once as the final top-level item
```
To express ITU-T H.264 clause 7.3.2.3.1 cleanly, `rbsp_trailing_bits;` must be permitted
as the terminal item of a conditional branch or switch case, while remaining forbidden
directly at repeat loop body top level.

### Relationship with `payload_size`

In standard H.264 streams, `payload_size` declared in `sei_message()` represents the byte
size of the payload up to the byte boundary produced by payload trailing bits. When
`payload_type == 6`, the structured fields and `rbsp_trailing_bits;` decode the message and
align the bit reader with the byte boundary at the end of the message. Other SEI payload
types that are not yet decoded structurally continue to use `@lazy(payload_size) bytes payload_data`,
which consumes exactly `payload_size` bytes and keeps unhandled messages opaque.

## Decision

1. **Recovery Point Syntax Elements**:
   In `SeiRbsp`, when `payload_type == 6`, decode the recovery point fields:
   - `ue recovery_frame_cnt`
   - `bits<1> exact_match_flag`
   - `bits<1> broken_link_flag`
   - `bits<2> changing_slice_group_idc @range(0, 2)`
   - `rbsp_trailing_bits;` to parse `bit_equal_to_one` and the zero alignment bits.

2. **Opaque Fallback for Other Types**:
   SEI payload types other than 6 remain opaque:
   ```svfmt
   if (payload_type == 6) {
       ue recovery_frame_cnt
           @spec("ITU-T H.264", "D.1.7, D.2.7")
           @description("Specifies the recovery frame count.");
       bits<1> exact_match_flag
           @spec("ITU-T H.264", "D.1.7, D.2.7")
           @description("Indicates whether decoding provides an exact match.");
       bits<1> broken_link_flag
           @spec("ITU-T H.264", "D.1.7, D.2.7")
           @description("Indicates whether the previous reference pictures may be missing.");
       bits<2> changing_slice_group_idc @range(0, 2)
           @spec("ITU-T H.264", "D.1.7, D.2.7")
           @description("Indicates whether changing slice groups are present.");
       rbsp_trailing_bits;
   } else {
       @lazy(payload_size)
       bytes payload_data
           @spec("ITU-T H.264", "7.3.2.3.1")
           @description("Carries the raw SEI message payload bytes.");
   }
   ```

3. **Conditional Trailing Bits Support in DSL**:
   Admit `rbsp_trailing_bits;` as the terminal statement of conditional branches
   inside structures. The compiler lowers it to `ReadRbspTrailingBits` guarded by
   the branch condition and iteration indices, and the VM executes the stop bit
   and zero alignment bit reads only when the branch is active.

4. **Package Version**:
   Upgrade `org.streamview.h264` package version from `0.1.31` to `0.1.32`.

## Consequences

- Recovery point SEI messages are fully decoded into typed syntax nodes with
  precise per-bit source spans, specifications, and descriptions.
- SEI messages following a recovery point message in the same NAL unit begin at
  the correct byte boundary without bit drift.
- Reserved values of `changing_slice_group_idc` (value 3) produce non-fatal
  `invalid-syntax` warning diagnostics per ADR-0036 and ADR-0076 while keeping the payload materialized.
- Unhandled SEI payload types remain safely opaque lazy byte regions.

## Follow-up

- ADR-0036: Enforce Range and Equals Domain Contracts on Fields
- ADR-0076: Signed Dynamic Range and Equals Domain Contracts
- ADR-0079: Accumulate Multi-Byte Numerical Values Using ff_coded
- ADR-0080: Bounded Iteration over RBSP Data with While-Repeat
