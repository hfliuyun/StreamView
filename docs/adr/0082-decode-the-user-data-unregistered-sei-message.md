# Decode the User Data Unregistered SEI Message

Status: Accepted
Date: 2026-08-15

## Context

ITU-T H.264 clause 7.3.2.3 specifies the Supplemental Enhancement Information (SEI)
RBSP container, and clause 7.3.2.3.1 defines the payload syntax dispatch. Clause
D.1.6 and clause D.2.6 define the User Data Unregistered SEI message (`payloadType == 5`),
which carries arbitrary user data identified by a standard UUID:

```text
user_data_unregistered( payloadSize ) {
    for( i = 0; i < 16; i++ )
        uuid_iso_iec_11578[ i ]                          u(8)
    for( i = 16; i < payloadSize; i++ )
        user_data_payload_byte                           b(8)
}
```

In clause D.2.6:
- `uuid_iso_iec_11578[ i ]`: shall have a value specified as a UUID according to ISO/IEC 11578:1996 Annex A (16 bytes, 128 bits).
- `user_data_payload_byte`: contains raw payload data bytes whose length is $payload\_size - 16$.

Because the 16 UUID bytes and the $(payload\_size - 16)$ user data bytes are both multiples of 8 bits, the payload is inherently byte-aligned. Per ITU-T H.264 clause 7.3.2.3.1, no payload trailing alignment bits are present.

### Probing and Language Capability Analysis

Probing in scratch confirmed that:
1. `switch (payload_type)` in `SeiRbsp` provides a clean, scalable dispatch structure matching NAL unit dispatch.
2. Fixed-length array syntax `bits<8> uuid_iso_iec_11578[16]` decomposes into 16 indexed byte nodes (`uuid_iso_iec_11578[0]` .. `uuid_iso_iec_11578[15]`).
3. Lazy byte expression `@lazy(payload_size - 16) bytes user_data_payload_byte` computes the remaining payload byte count dynamically.
4. When $payload\_size < 16$, arithmetic subtraction underflow produces an `invalid-syntax` diagnostic and rolls back the transaction cleanly.
5. All semantics are expressible with existing verified DSL capabilities without modifying the DSL compiler or virtual machine.

## Decision

1. **User Data Unregistered Syntax Elements**:
   In `SeiRbsp`, when `payload_type == 5`, decode:
   - `bits<8> uuid_iso_iec_11578[16]`
   - `@lazy(payload_size - 16) bytes user_data_payload_byte`

2. **Switch-Based SEI Payload Dispatch**:
   Refactor `SeiRbsp` payload dispatch from `if/else` to `switch (payload_type)`:
   ```svfmt
   switch (payload_type) {
       case 5: {
           bits<8> uuid_iso_iec_11578[16]
               @spec("ITU-T H.264", "D.1.6, D.2.6")
               @description("Specifies the UUID identifying the syntax and semantics of the unregistered user data.");
           @lazy(payload_size - 16)
           bytes user_data_payload_byte
               @spec("ITU-T H.264", "D.1.6, D.2.6")
               @description("Carries the raw unregistered user data bytes.");
       }
       case 6: {
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
       }
       default: {
           @lazy(payload_size)
           bytes payload_data
               @spec("ITU-T H.264", "7.3.2.3.1")
               @description("Carries the raw SEI message payload bytes.");
       }
   }
   ```

3. **Package Version**:
   Upgrade `org.streamview.h264` package version from `0.1.32` to `0.1.33`.

## Consequences

- User data unregistered SEI messages are decoded with exact 16-byte UUID arrays and lazy user data payload bytes.
- When $payload\_size == 16$, the user data payload byte region materializes with length 0 at the byte boundary.
- Unregistered user data with $payload\_size < 16$ is rejected as invalid syntax via subtraction underflow protection.
- SEI payload dispatch is structured as an extensible `switch` statement for subsequent payload type implementations (T9–T12).
- Unhandled payload types continue to decode as opaque lazy byte regions via the `default` arm.

## Follow-up

- ADR-0036: Enforce Range and Equals Domain Contracts on Fields
- ADR-0079: Accumulate Multi-Byte Numerical Values Using ff_coded
- ADR-0080: Bounded Iteration over RBSP Data with While-Repeat
- ADR-0081: Decode the Recovery Point SEI Message
