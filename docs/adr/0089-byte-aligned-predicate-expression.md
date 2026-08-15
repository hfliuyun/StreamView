# ADR-0089: Byte-Aligned Predicate Expression

## Status

Accepted

## Context

In ITU-T H.264 clause 7.3.2.3.1 (`sei_payload`), SEI message alignment bits are specified as conditional on whether the bit reader is currently byte-aligned:

```text
if( !byte_aligned( ) ) {
    bit_equal_to_one /* equal to 1 */
    while( !byte_aligned( ) )
        bit_equal_to_zero /* equal to 0 */
}
```

In previously implemented SEI messages (e.g. `buffering_period`, `recovery_point`, `display_orientation`), the total payload bit width was mathematically guaranteed to never be a multiple of 8 due to odd-width `ue` or odd flag counts. Consequently, `!byte_aligned()` was invariant true, allowing an unconditional trailing `rbsp_trailing_bits;` statement to consume the 1..7 padding bits.

However, in SEI messages such as `pic_timing` (ITU-T H.264 D.1.2 / D.2.2), when `CpbDpbDelaysPresentFlag` is active with default 24-bit delays (24 + 24 = 48 bits) and `pic_struct_present_flag == 0`, the total payload length is exactly 48 bits (6 bytes, an integer multiple of 8). In standard conforming bitstreams, no alignment bits are emitted. If an unconditional `rbsp_trailing_bits;` statement is evaluated at the end of the message, the virtual machine opcode `ReadRbspTrailingBits` will unconditionally attempt to read 8 bits (`10000000`), consuming the next SEI payload type or NAL trailing bits and desynchronizing the stream.

Probing of the current DSL rules engine demonstrates that:
1. Conditional branches containing `rbsp_trailing_bits` (`if (condition) { rbsp_trailing_bits; }`) are already fully supported by the parser (`src/rules/dsl.cpp:1531`), IR lowering (`src/rules/dsl_ir.cpp:1076`), and VM execution (`src/rules/dsl_vm.cpp:2805-2814`). When the condition is false, the VM cleanly skips the trailing bits without reading any bits from the stream.
2. The expression language currently only provides `more_rbsp_data()` to query stream termination state, but lacks a stream alignment predicate `byte_aligned()`.

## Decision

We introduce a built-in nullary boolean expression `byte_aligned()` to the DSL expression grammar, static semantics, and bytecode VM runtime:

1. **Syntax & Grammar**:
   - `byte_aligned()` is parsed as a nullary call expression returning `DslScalarType::Bool`.
   - Disallowed in pure functions (`src/rules/dsl.cpp`), because pure functions operate over scalar values and do not possess an ambient `BitReader` / bitstream context (identical to `more_rbsp_data()`).
2. **Typed IR Lowering**:
   - Lowered to `DslTypedExpressionKind::ByteAligned` (`src/rules/dsl_ir.cpp`) with scalar type `DslScalarType::Bool`.
3. **VM Evaluation & Coordinate System**:
   - Evaluated as `(logicalStart + reader.position()) % 8 == 0` (`src/rules/dsl_vm.cpp`).
   - `logicalStart` is the absolute logical bit address of the current structure within the `BitReader` / `AnalysisSource` logical coordinate space.
   - When reading through an EBSP-to-RBSP mapped slice or a sub-region, `logicalStart + reader.position()` accurately tracks the current logical bit coordinate.
   - For `@lazy` byte regions, regions are byte-aligned by construction per ADR-0026, preserving invariant coordinate alignment.
4. **Format-Agnostic Design**:
   - The expression is completely format-neutral and carries zero format-specific or SEI-specific semantics in the core or DSL runtime.
5. **Rule Consumption**:
   - Format rules express conditional SEI payload alignment via intermediate boolean computed fields (`src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt:894-898`):
     ```svfmt
     computed<bool> is_aligned = byte_aligned();
     computed<bool> needs_trailing_bits = !is_aligned;
     if (needs_trailing_bits) {
         rbsp_trailing_bits;
     }
     ```

## Consequences

### Positive
- Fully and faithfully models ITU-T H.264 clause 7.3.2.3.1 alignment semantics via boolean computed variables (`computed<bool> is_aligned = byte_aligned(); computed<bool> needs_trailing_bits = !is_aligned; if (needs_trailing_bits) { rbsp_trailing_bits; }`).
- Enables `pic_timing` and any future format messages to safely support both byte-aligned and unaligned payloads without bitstream desynchronization.
- Reuses existing VM `ReadRbspTrailingBits` logic without altering opcode semantics.

### Negative / Trade-offs
- Adds one new built-in identifier `byte_aligned` to the expression language.

### Capability Verification Matrix
- `byte_aligned()` evaluates to true at bit offsets 0, 8, 16, 24, 32, 40, 48, etc.
- `byte_aligned()` evaluates to false at bit offsets 1..7, 9..15, etc.
- Evaluates correctly inside `repeat`, `switch`, and `if` conditional scopes.
- Evaluates correctly immediately following `@lazy` byte regions.
- Verified that direct condition syntax `if (!byte_aligned())` is blocked by the condition parser grammar gate (`src/rules/dsl.cpp:1180`, emitting `Conditions require a field or context_value equality`), while the canonical landed pattern via boolean computed fields (`computed<bool> is_aligned = byte_aligned(); computed<bool> needs_trailing_bits = !is_aligned; if (needs_trailing_bits) { rbsp_trailing_bits; }`) compiles and executes with full alignment precision.

