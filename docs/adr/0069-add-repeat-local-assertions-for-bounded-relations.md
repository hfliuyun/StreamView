# Add Repeat-Local Assertions for Bounded Relations

Status: Accepted
Date: 2026-08-10

## Context

The bounded H.264 reference-picture marking loop now decodes every operand
shape, but clause 7.4.3.3 still has relationships that apply to each decoded
operation. The existing `assert(condition) at anchor;` statement is deliberately
top-level only. Placing an assertion after a sentinel repeat cannot refer to a
repeat projection: the projection is expanded into indexed fields and a value
that was not materialized on one branch is not a valid dependency.

Moving this check into the H.264 analyzer would make the official rule's
conformance behavior format-specific C++ and would lose the source-anchored
diagnostic contract of the DSL.

## Decision

Allow `assert(condition) at anchor;` inside a bounded or sentinel repeat,
including its conditional and switch bodies. Such an assertion is a
**repeat-local assertion**. The compiler expands it once per statically
projected iteration, so the condition may reference only earlier scalar fields
from that same iteration and values guaranteed by the active branch. The
anchor is the earlier source-backed scalar field from that iteration.

The statement remains invalid outside the top level or a repeat-local scope.
It adds no field or presentation node. The typed descriptor stores the active
field conditions alongside its condition and anchor. The VM evaluates those
conditions before the assertion; when the branch is not selected it skips the
assertion, and when it is selected it evaluates the Boolean expression without
reading source or moving the cursor. A false condition is fatal
`invalid-syntax` with the existing `Assertion condition is false` message and
the current iteration's complete anchor range. Checked expression failures
retain their existing status and diagnostic behavior.

The descriptor and bytecode preflight still enforce declaration order,
descriptor cardinality, field positions, operand/immediate values, and the
condition/anchor dependency relationship. Assertions count toward the
structure's 1,024 assertion limit and the instruction budget; skipped
assertions remain cancellation points.

The official H.264 rule uses this capability for three bounded checks against
the imported SPS `max_num_ref_frames` value: operation 2 requires
`long_term_pic_num_mmco < max_num_ref_frames`, operations 3 and 6 require
`long_term_frame_idx < max_num_ref_frames`, and operation 4 requires
`max_long_term_frame_idx_plus1 <= max_num_ref_frames`. The SPS export and
these assertions are published by package `0.1.22` at coverage depth
`relational-marking-slice-header`. The short-term operation 1/3
`difference_of_pic_nums_minus1` relation remains deferred because its correct
MaxPicNum bound needs additional frame-number context. This increment does not
track a decoded-picture buffer, model operation ordering, detect duplicate or
contradictory operations, or claim complete clause 7.4.3.3 conformance.

## Consequences

Rules can own source-anchored relationships that apply to bounded projections,
while the VM remains format-neutral. A failed operation preserves the already
materialized prefix and allows the Annex B analyzer to continue with later NAL
units. The field namespace remains flat: repeat projections keep their indexed
presentation names and no runtime array value is introduced.

## Non-goals

This decision does not add post-repeat aggregate expressions, mutable rule
state, array indexing, warning assertions, custom assertion messages, or DPB
simulation. Cross-iteration and cross-NAL relationships remain deferred.
