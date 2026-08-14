# Bind Redefined Parameter Sets By Stream Position

Status: Accepted
Date: 2026-08-14

## Context

ITU-T H.264 clause 7.4.1.2 permits a Sequence Parameter Set (SPS) or Picture
Parameter Set (PPS) sharing an existing `seq_parameter_set_id` or
`pic_parameter_set_id` to be redefined later in the bitstream. Slices decoded
after the redefinition point activate the newly published parameter set, while
slices preceding the redefinition point must remain bound to the prior
generation.

ADR-0028 defined position-based resolution in `ContextDirectory`, ADR-0044 and
ADR-0045 integrated declarative `@context_export` and `@context_import`
lifecycle in the DSL runtime, and ADR-0073 verified that PPS extensions bind to
the active SPS generation without selecting future or stale generations.

However, format-level end-to-end slice header binding across mid-stream parameter
set redefinitions—including dynamic field bit widths derived from context values
(such as `frame_num` width calculated from `log2_max_frame_num_minus4 + 4`)—was
not yet locked with dedicated regression fixtures in the Annex B analyzer suite.

## Decision

Formally lock end-to-end format acceptance for in-stream parameter set
redefinition and positional generation binding in the bundled H.264 analyzer:

1. **Positional generation activation**: When an SPS is redefined (e.g. changing
   `log2_max_frame_num_minus4` from 0 to 2) and bound through a subsequent PPS,
   subsequent slice headers dynamically evaluate context expressions against the
   newly published generation, producing widened fields (e.g. `frame_num`
   widening from 4 bits to 6 bits) without syntax errors or boundary shifts.
2. **Prior generation stability**: Slices analyzed before the redefinition
   remain bound to their original generation, preserving their materialized
   structure, exact field bit lengths, and diagnostic-free state.
3. **Invalid redefinition isolation**: An invalid or malformed parameter set NAL
   (e.g., an SPS with a reserved profile IDC) fails with an `invalid` state and
   does not publish or corrupt the active generation table. Parameter sets and
   slices referencing that invalid definition fail with `dependency-unavailable`
   diagnostics, while prior valid slices remain fully materialized and intact.

## Consequences

End-to-end regression tests verify:

- Positive stream: `SPS(id 0, log2_max_frame_num_minus4=0)` → `PPS(id 0)` →
  `Slice A (frame_num=4 bits)` → `SPS(id 0, log2_max_frame_num_minus4=2)` →
  `PPS(id 0)` → `Slice B (frame_num=6 bits)` → following `AUD`, confirming both
  slices decode with exact distinct dynamic widths and complete ordered child
  structures;
- Negative stream: `SPS(id 0, valid)` → `PPS(id 0)` → `Slice A` →
  `SPS(id 1, invalid profile)` → `PPS(id 1)` → `Slice B` → following `AUD`,
  confirming that Slice A remains completely valid and materialized while
  Slice B reports `dependency-unavailable`.

Phase 3 item 5 ("支持同 ID SPS/PPS 中途重定义和按位置选择") is satisfied.

## Non-goals

This decision does not implement dynamic Decoded Picture Buffer (DPB) management,
IDR sequence resets, recovery point synchronization, or multi-slice group
redefinition.

## Follow-up

- ADR-0028: Resolve Context Generations By Source Position
- ADR-0044: Publish Rule-Declared Context Generations
- ADR-0045: Import Rule-Declared Context Generations
- ADR-0073: Decode The Bounded High Profile PPS Extension
