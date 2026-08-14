# Iterate Bounded Repeats Over RBSP Data

Status: Accepted
Date: 2026-08-14

## Context

ITU-T H.264 clause 7.3.2.3 specifies Supplemental Enhancement Information (SEI)
RBSP as a sequence of SEI messages repeating while RBSP data remains:

```text
sei_rbsp( ) {
    do
        sei_message( )
    while( more_rbsp_data( ) )
    rbsp_trailing_bits( )
}
```

Probing current StreamView DSL capabilities with `svtool rule check` confirmed
that this loop structure cannot be expressed using existing constructs:

1. Sentinel repeat loops (`repeat (N) { ... } until (field == value)`) require
   a source-backed scalar field defined directly inside the repeat body (ADR-0070).
2. Attempting to use `more_rbsp_data()` in a sentinel `until` clause produces:
   ```text
   error: Sentinel field must be declared directly in the repeat body
   ```
3. Attempting to use a computed boolean (`computed<bool> is_done = more_rbsp_data() == false;`)
   in a sentinel `until` clause also fails:
   ```text
   error: Sentinel field must be declared directly in the repeat body
   ```

To support parsing SEI message containers (and analogous repeating structures
terminated by RBSP boundaries), the DSL requires a bounded loop form driven by
the stream's remaining RBSP data state.

## Decision

Introduce a bounded RBSP-data-driven repeat loop into the StreamView DSL:

1. **Syntax**:
   ```svfmt
   repeat (64) while (more_rbsp_data()) {
       SeiMessage message;
   }
   ```
   `max_iterations` (e.g. 64) is a required compile-time positive integer literal
   bounding the loop (`1 <= max_iterations <= 1024`). `while (more_rbsp_data())`
   specifies the iteration condition.

2. **Execution Semantics**:
   - Before executing each iteration (including the first iteration), the VM
     evaluates `more_rbsp_data()`.
   - If `more_rbsp_data()` evaluates to `true`, the iteration's child fields
     are decoded sequentially.
   - If `more_rbsp_data()` evaluates to `false`, the loop terminates cleanly,
     and execution proceeds to subsequent fields (e.g. `rbsp_trailing_bits`).
   - If `max_iterations` iterations have executed and `more_rbsp_data()` still
     evaluates to `true`, decoding fails with an `invalid-syntax` diagnostic
     indicating that the loop exceeded its declared bound.
   - If an iteration encounters a truncation or syntax failure within its body,
     the loop halts and propagates the failure.

3. **Typed IR and Bytecode**:
   - Represented as a distinct typed repeat form `DslTypedWhileRepeat` (or
     extended `DslTypedRepeat`) with `maximumIterations` and condition
     expression `more_rbsp_data()`.
   - The compiler emits conditional evaluation opcodes before each loop
     iteration, jumping past the repeat block when `more_rbsp_data()` is false.

## Consequences

- H.264 SEI RBSP (clause 7.3.2.3) message iteration can be expressed
  declaratively without synthetic sentinel fields or manual message counting.
- Execution remains strictly bounded by `max_iterations`, preventing infinite
  loops or runaway node materialization.
- The DSL type system and VM execution model remain deterministic and
  cancellation-safe.

## Non-goals

- This decision does not introduce arbitrary unbounded while loops or general
  runtime while conditions. `more_rbsp_data()` is the only accepted loop
  condition predicate.

## Follow-up

- ADR-0070: Match List Modification Terminators With Sentinel Repeats
- ADR-0079: Encode Accumulated Byte Values With ff_coded
