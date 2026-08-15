# ADR-0088: Decode the Display Orientation SEI Message

- **Status**: Accepted
- **Date**: 2026-08-15
- **Deciders**: StreamView Core Team

## Context

The H.264 Annex B official rules package (`org.streamview.h264`) decodes several SEI message payload types (such as `buffering_period` (0), `user_data_registered_itu_t_t35` (4), `user_data_unregistered` (5), `recovery_point` (6), and `frame_packing_arrangement` (45)). Other SEI payload types are currently deferred to a fallback branch `@lazy(payload_size) bytes payload_data`.

The **Display Orientation SEI message** (`payload_type == 47`, ITU-T H.264 clauses D.1.27 and D.2.27) provides information to assist decoders and renderers in transforming the decoded picture before display (e.g. horizontal flipping, vertical flipping, and anti-clockwise rotation degrees).

Because the syntax elements of the display orientation message are self-contained within its payload and do not depend on external parameter set contexts, this message can be decoded directly using standard DSL integer types and conditional statements.

## Decision

We implement full structured decoding for the Display Orientation SEI message (`payload_type == 47`) in the official H.264 rules package and bump the package version to `0.1.37`.

### 1. Syntax Mapping

According to ITU-T H.264 clauses D.1.27 and D.2.27:

- `display_orientation_cancel_flag`: `bits<1>` (indicates whether the message cancels the persistence of any previous display orientation SEI message).
- When `display_orientation_cancel_flag == 0`:
  - `hor_flip`: `bits<1>` (indicates whether the intended display is horizontally flipped).
  - `ver_flip`: `bits<1>` (indicates whether the intended display is vertically flipped).
  - `anticlockwise_rotation`: `bits<16>` (specifies the intended anti-clockwise rotation degree in units of $2^{-16}$ of a degree, representing 0 to $360^\circ - 2^{-16\circ}$).
  - `display_orientation_repetition_period`: `ue @range(0, 16384)` (specifies repetition persistence in range 0..16384).
  - `display_orientation_extension_flag`: `bits<1> @range(0, 0)` (reserved extension flag conforming to 0).
- `rbsp_trailing_bits`: Standard byte alignment trailing bits.

### 2. Node Hierarchy and Naming

All syntax elements are materialized as direct children of `SeiRbsp` within the `case 47:` branch of the `switch (payload_type)`. Field names strictly match the ITU-T H.264 standard syntax specification.

### 3. Error Handling and Isolation

- Any truncated payload triggers analyzer backtrack and reports a diagnostics warning while allowing subsequent NAL units to continue parsing cleanly.
- Out-of-range values on `@range` annotations generate standard non-fatal validation diagnostics.

## Consequences

- The official `org.streamview.h264` rule package version increases from `0.1.36` to `0.1.37`.
- Display orientation SEI messages are fully inspected with bit-exact source spans and syntax validation.

## References

- ITU-T H.264 Clauses D.1.27, D.2.27
- ADR-0087: Decode the Frame Packing Arrangement SEI Message
