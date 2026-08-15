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

To allow chained SEI messages within the same NAL unit to parse starting at clean
byte boundaries and to accurately reflect the decoded recovery point semantics, the
rule must decode the recovery point fields and consume the payload trailing bits.

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
   the branch condition, and the VM executes the stop bit and zero alignment bit
   reads only when the branch is active.

4. **Package Version**:
   Upgrade `org.streamview.h264` package version from `0.1.31` to `0.1.32`.

## Consequences

- Recovery point SEI messages are fully decoded into typed syntax nodes with
  precise per-bit source spans, specifications, and descriptions.
- SEI messages following a recovery point message in the same NAL unit begin at
  the correct byte boundary without bit drift.
- Reserved values of `changing_slice_group_idc` (value 3) produce non-fatal
  `invalid-syntax` diagnostics per ADR-0040 while keeping the payload materialized.
- Unhandled SEI payload types remain safely opaque lazy byte regions.

## Follow-up

- ADR-0040: Report ue Range Violations Without Stopping Decoding
- ADR-0079: Encode Accumulated Byte Values With ff_coded
- ADR-0080: Iterate Bounded Repeats Over RBSP Data
