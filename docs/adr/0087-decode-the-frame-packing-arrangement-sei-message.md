# ADR-0087: Decode the Frame Packing Arrangement SEI Message

- **Status**: Accepted
- **Date**: 2026-08-15
- **Deciders**: StreamView Core Team

## Context

The H.264 Annex B official rules package (`org.streamview.h264`) decodes several SEI message payload types (such as `buffering_period` (0), `user_data_registered_itu_t_t35` (4), `user_data_unregistered` (5), and `recovery_point` (6)). Other SEI payload types are currently deferred to a fallback branch `@lazy(payload_size) bytes payload_data`.

The **Frame Packing Arrangement SEI message** (`payload_type == 45`, ITU-T H.264 clauses D.1.25 and D.2.25) provides information to assist decoders in rearranging constituent frames of a stereoscopic 3D video sequence (e.g. side-by-side, top-and-bottom, checkboard, or frame sequential formats).

Because the syntax elements of the frame packing arrangement message are fully self-contained within its payload and do not depend on external SPS or PPS contexts, this message can be decoded directly using standard DSL integer types, conditional statements, and computed boolean expressions.

## Decision

We implement full structured decoding for the Frame Packing Arrangement SEI message (`payload_type == 45`) in the official H.264 rules package and bump the package version to `0.1.36`.

### 1. Syntax Mapping

According to ITU-T H.264 clauses D.1.25 and D.2.25:

- `frame_packing_arrangement_id`: `ue` (unsigned Exp-Golomb integer identifying the message usage).
- `frame_packing_arrangement_cancel_flag`: `bits<1>` (indicates whether the message cancels the persistence of previous messages).
- When `frame_packing_arrangement_cancel_flag == 0`:
  - `frame_packing_arrangement_type`: `bits<7> @range(0, 7)` (specifies the packing arrangement type: 0 checkerboard, 1 column interleaving, 2 row interleaving, 3 side-by-side, 4 top-and-bottom, 5 frame sequential, 6 2D frame, 7 tiled).
  - `quincunx_sampling_flag`: `bits<1>` (indicates whether each constituent frame is quincunx sampled).
  - `content_interpretation_type`: `bits<6> @range(0, 2)` (0: unspecified, 1: frame 0 left / frame 1 right, 2: frame 0 right / frame 1 left).
  - `spatial_flipping_flag`: `bits<1>` (indicates whether spatial flipping is performed).
  - `frame0_flipped_flag`: `bits<1>` (indicates whether frame 0 has been flipped).
  - `field_views_flag`: `bits<1>` (indicates whether all pictures in the sequence are field pictures).
  - `current_frame_is_frame0_flag`: `bits<1>` (indicates whether current decoded picture corresponds to frame 0).
  - `frame0_self_contained_flag`: `bits<1>` (indicates whether frame 0 is self-contained).
  - `frame1_self_contained_flag`: `bits<1>` (indicates whether frame 1 is self-contained).
  - `has_grid_position`: `computed<bool> = quincunx_sampling_flag == 0 && frame_packing_arrangement_type != 5;`
    - If `has_grid_position`:
      - `frame0_grid_position_x`: `bits<4> @range(0, 15)`
      - `frame0_grid_position_y`: `bits<4> @range(0, 15)`
      - `frame1_grid_position_x`: `bits<4> @range(0, 15)`
      - `frame1_grid_position_y`: `bits<4> @range(0, 15)`
  - `frame_packing_arrangement_reserved_byte`: `bits<8> @range(0, 0)` (reserved byte conforming to 0).
  - `frame_packing_arrangement_repetition_period`: `ue @range(0, 16384)` (specifies repetition persistence in range 0..16384).
- `frame_packing_arrangement_extension_flag`: `bits<1> @range(0, 0)` (reserved extension flag conforming to 0).
- `rbsp_trailing_bits`: Standard byte alignment trailing bits.

### 2. Node Hierarchy and Naming

All syntax elements are materialized as direct children of `SeiRbsp` within the `case 45:` branch of the `switch (payload_type)`. Field names strictly match the ITU-T H.264 standard syntax specification.

### 3. Error Handling and Isolation

- Any truncated payload triggers analyzer backtrack and reports a diagnostics warning while allowing subsequent NAL units to continue parsing cleanly.
- Out-of-range values on `@range` annotations generate standard non-fatal validation diagnostics.

## Consequences

- The official `org.streamview.h264` rule package version increases from `0.1.35` to `0.1.36`.
- Frame packing arrangement SEI messages are fully inspected with bit-exact source spans and syntax validation.

## References

- ITU-T H.264 Clauses D.1.25, D.2.25
- ADR-0085: Decode the Buffering Period SEI Message
- ADR-0086: Ambient Context Imports and Active Parameter Set Resolution
