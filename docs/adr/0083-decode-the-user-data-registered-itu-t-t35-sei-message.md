# Decode the User Data Registered ITU-T T.35 SEI Message

Status: Accepted
Date: 2026-08-15

## Context

ITU-T H.264 clause 7.3.2.3 specifies the Supplemental Enhancement Information (SEI)
RBSP container, and clause 7.3.2.3.1 defines the payload syntax dispatch. Clause
D.1.5 and clause D.2.5 define the User Data Registered by Recommendation ITU-T T.35
SEI message (`payloadType == 4`), which carries arbitrary user data identified by a
registered country code:

```text
user_data_registered_itu_t_t35( payloadSize ) {
    itu_t_t35_country_code                          b(8)
    if( itu_t_t35_country_code != 0xFF )
        i = 1
    else {
        itu_t_t35_country_code_extension_byte       b(8)
        i = 2
    }
    do {
        itu_t_t35_payload_byte                      b(8)
        i++
    } while( i < payloadSize )
}
```

In clause D.2.5:
- `itu_t_t35_country_code`: an 8-bit field specifying a country code according to ITU-T Recommendation T.35 Annex A. When its value is `0xFF` (255), the subsequent `itu_t_t35_country_code_extension_byte` specifies the extended country code.
- `itu_t_t35_country_code_extension_byte`: an 8-bit field specifying the extension country code when `itu_t_t35_country_code == 0xFF`.
- `itu_t_t35_payload_byte`: data bytes carrying the registered payload whose syntax and semantics are determined by the country code authority (e.g. ATSC, CTA, DVB, HDR10+).

Because the header consists of 1 or 2 whole bytes, and the payload is a sequence of whole bytes $(payload\_size - 1)$ or $(payload\_size - 2)$, the message is inherently byte-aligned. Per ITU-T H.264 clause 7.3.2.3.1, no payload trailing alignment bits are present.

### Probing and Language Capability Analysis

Probing in scratch confirmed that:
1. `case 4` in `SeiRbsp` integrates into the existing `switch (payload_type)` dispatch table.
2. In the DSL, struct field names must be unique across all branches within a struct. The conditional extension branch defines `bits<8> itu_t_t35_country_code_extension_byte` and `@lazy(payload_size - 2) bytes itu_t_t35_extension_payload_byte`, while the standard non-extension branch defines `@lazy(payload_size - 1) bytes itu_t_t35_payload_byte`.
3. When `payload_size < 1` (or `payload_size < 2` when `itu_t_t35_country_code == 255`), subtraction underflow produces an `invalid-syntax` diagnostic and rolls back the transaction safely.
4. All semantics are expressible with existing verified DSL capabilities without modifying the DSL compiler or virtual machine.

## Decision

1. **User Data Registered ITU-T T.35 Syntax Elements**:
   In `SeiRbsp`, when `payload_type == 4`, decode:
   ```svfmt
   case 4: {
       bits<8> itu_t_t35_country_code
           @spec("ITU-T H.264", "D.1.5, D.2.5")
           @description("Specifies the ITU-T Recommendation T.35 country code.");
       if (itu_t_t35_country_code == 255) {
           bits<8> itu_t_t35_country_code_extension_byte
               @spec("ITU-T H.264", "D.1.5, D.2.5")
               @description("Specifies the ITU-T Recommendation T.35 country code extension byte.");
           @lazy(payload_size - 2)
           bytes itu_t_t35_extension_payload_byte
               @spec("ITU-T H.264", "D.1.5, D.2.5")
               @description("Carries the raw ITU-T Recommendation T.35 payload bytes when an extension byte is present.");
       } else {
           @lazy(payload_size - 1)
           bytes itu_t_t35_payload_byte
               @spec("ITU-T H.264", "D.1.5, D.2.5")
               @description("Carries the raw ITU-T Recommendation T.35 payload bytes.");
       }
   }
   ```

2. **Package Version**:
   Upgrade `org.streamview.h264` package version from `0.1.33` to `0.1.34`.

## Consequences

- User data registered ITU-T T.35 SEI messages are decoded with exact country code fields and lazy payload bytes.
- Standard country codes (`country_code != 255`) materialize `itu_t_t35_country_code` and `itu_t_t35_payload_byte`.
- Extended country codes (`country_code == 255`) materialize `itu_t_t35_country_code`, `itu_t_t35_country_code_extension_byte`, and `itu_t_t35_extension_payload_byte`.
- Zero-length payload configurations (`payload_size == 1` for standard, `payload_size == 2` for extension) materialize 0-length byte regions at the byte boundary.
- Underflow configurations (`payload_size == 0` for standard, `payload_size < 2` for extension) are rejected as invalid syntax via subtraction underflow protection.

## Follow-up

- ADR-0036: Enforce Range and Equals Domain Contracts on Fields
- ADR-0079: Accumulate Multi-Byte Numerical Values Using ff_coded
- ADR-0080: Bounded Iteration over RBSP Data with While-Repeat
- ADR-0081: Decode the Recovery Point SEI Message
- ADR-0082: Decode the User Data Unregistered SEI Message
