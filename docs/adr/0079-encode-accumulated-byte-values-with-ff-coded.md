# Encode Accumulated Byte Values With ff_coded

Status: Accepted
Date: 2026-08-14

## Context

ITU-T H.264 clause 7.3.2.3.1 (and analogous specifications in HEVC, VVC, and
AV1) defines Supplemental Enhancement Information (SEI) message headers using
variable-length byte accumulation loops for `payloadType` and `payloadSize`:

```text
while( next_bits( 8 ) == 0xFF ) {
    ff_byte /* equal to 0xFF */
    payloadType += 255
}
last_payload_type_byte
payloadType += last_payload_type_byte
```

Probing current StreamView DSL capabilities with `svtool rule check` confirmed
that this syntax cannot be expressed using existing constructs:

1. Sentinel repeat loops (`repeat (N) { ... } until (sentinel == value)`) only
   support equality comparisons against literal constants (ADR-0070). Attempting
   `until (ff_byte != 255)` produces:
   ```text
   error: Expected '==' after sentinel field name
   ```
2. The DSL does not support reduction expressions across repeated elements
   (such as `sum(ff_byte)`). Attempting to reference repeated elements in a
   computed field produces:
   ```text
   error: Computed field dependency must be declared earlier
   error: Pure function is not declared before this call
   ```

`payloadType` and `payloadSize` represent scalar integer syntax elements
carrying unsigned numeric values. Expressing them via array repeats and reduction
operators would introduce unnecessary AST fragmentation and non-local sum
arithmetic.

## Decision

Introduce a native scalar field encoding `ff_coded<max_bytes>` into the StreamView
DSL:

1. **Syntax**:
   ```svfmt
   ff_coded<8> payload_type
       @description("Carries the SEI message payload type.");
   ff_coded<8> payload_size
       @description("Carries the SEI message payload size in bytes.");
   ```
   `max_bytes` is a required compile-time positive integer literal bounding the
   maximum number of bytes permitted for the field (`1 <= max_bytes <= 64`).

2. **Decoding Semantics**:
   - The decoder reads 8-bit bytes sequentially.
   - For each byte equal to `0xFF` (255), it adds 255 to the running accumulator
     and continues to the next byte.
   - The first byte with a numeric value less than `0xFF` adds its value to the
     accumulator and terminates the field decode.
   - If `max_bytes` bytes are read and the final byte is still `0xFF`, decoding
     fails with an `invalid-syntax` diagnostic.
   - If fewer than 8 bits remain before reading the terminating byte, decoding
     fails with a truncation diagnostic.

3. **AST Representation**:
   - The field produces a single scalar `u64` AST node carrying the accumulated
     numeric value (`255 * N + last_byte`).
   - The field's source span covers the exact contiguous byte range consumed
     (all `0xFF` prefix bytes plus the terminating byte), matching the mapped
     span semantics of `ue` and `se`.

## Consequences

- `payloadType` and `payloadSize` syntax elements in H.264 SEI messages (clause
  7.3.2.3.1) can be declared declaratively as single scalar fields.
- The DSL avoids introducing general-purpose loop accumulation or array reduction
  complexity.
- Byte consumption remains strictly bounded and deterministic.

## Non-goals

- This decision does not introduce arbitrary-radix byte accumulation or general
  loop reduction arithmetic.
- Outer message loop repetition (`while( more_rbsp_data() )`) is addressed in a
  separate design decision (ADR-0080).

## Follow-up

- ADR-0070: Match List Modification Terminators With Sentinel Repeats
- ADR-0080: Iterate Bounded Repeats Over RBSP Data
