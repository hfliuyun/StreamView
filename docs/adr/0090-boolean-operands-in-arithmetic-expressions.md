# ADR-0090: Boolean Operands in Additive and Multiplicative Arithmetic Expressions

## Status

Accepted

## Context

In video coding specifications such as ITU-T H.264 Table D-1 (`pic_struct` to `NumClockTS` mapping), piecewise tabular functions are conventionally expressed as indicator-weighted sums:

$$\text{NumClockTS} = (pic\_struct \le 2) \cdot 1 + (pic\_struct \in \{3, 4, 7\}) \cdot 2 + (pic\_struct \in \{5, 6, 8\}) \cdot 3$$

In the format description language, writing pure functions or computed expressions of this form:
```svfmt
pure u64 num_clock_ts_for_pic_struct(u64 pic_struct) {
    return (pic_struct <= 2) * 1 +
           (pic_struct == 3 || pic_struct == 4 || pic_struct == 7) * 2 +
           (pic_struct == 5 || pic_struct == 6 || pic_struct == 8) * 3;
}
```
is currently rejected by the static type checker across two compiler gates:
1. **Parser Layer Checkpoint**: `src/rules/dsl.cpp:2051-2058` checks `scalarType(operand) != DslScalarType::U64` in `requireOperands`, and emits diagnostic `Arithmetic operators require u64 operands` at `src/rules/dsl.cpp:2066`.
2. **Typed IR Layer Checkpoint**: `src/rules/dsl_ir.cpp:897` enforces `typesValid = left->type == DslScalarType::U64 && right->type == DslScalarType::U64;`, emitting `Expression operand types do not match the operator` at `src/rules/dsl_ir.cpp:930`.

Because relational and logical operators (`<=`, `==`, `||`) yield `DslScalarType::Bool`, and the type system previously prohibited mixing `Bool` and `U64` in arithmetic operators, indicator arithmetic could not be expressed.

## Decision

We relax operand typing rules strictly for the commutative and monotonic arithmetic operators `+` (`Add`) and `*` (`Multiply`), allowing operands of type `DslScalarType::Bool` with automatic coercion to `0ULL` or `1ULL`:

1. **Scope of Relaxation (Strictly `+` and `*`)**:
   - `+` (`Add`) and `*` (`Multiply`) accept operands where each operand is independently `DslScalarType::U64` or `DslScalarType::Bool`.
   - The result type is always `DslScalarType::U64`.
   - Non-relaxed arithmetic operators:
     - `-` (`Subtract`): Continues to strictly require `U64` operands. Coercing boolean operands in subtraction (e.g. `false - 1` or `false - true`) introduces immediate unsigned 64-bit integer underflow ($2^{64}-1$), violating arithmetic safety.
     - `/` (`Divide`) and `%` (`Remainder`): Continue to strictly require `U64` operands. Coercing boolean operands in division/remainder introduces silent runtime division by zero when the operand is `false` (`/ 0`).
2. **Coercion & Boundary Semantics**:
   - `true` coerces to `1ULL`; `false` coerces to `0ULL`.
   - `bool + bool` $\to$ `u64` (e.g. `true + true == 2`, `true + false == 1`, `false + false == 0`).
   - `bool * bool` $\to$ `u64` (e.g. `true * true == 1`, `true * false == 0`).
   - `bool + u64` / `u64 + bool` $\to$ `u64`.
   - `bool * u64` / `u64 * bool` $\to$ `u64`.
3. **Dual-Gate Implementation**:
   - **Parser Layer** (`src/rules/dsl.cpp:2059-2067`):
     - For `Add` and `Multiply`, validate that `leftType` and `rightType` are each `U64` or `Bool`.
     - For `Subtract`, `Divide`, `Remainder`, require `U64` operands with diagnostic `Subtraction, division, and remainder operators require u64 operands`.
   - **Typed IR Layer** (`src/rules/dsl_ir.cpp:891-900`):
     - For `Add` and `Multiply`, set `typesValid` to true when `left` and `right` are in `{U64, Bool}`, with `resultType = DslScalarType::U64`.
     - For `Subtract`, `Divide`, `Remainder`, retain `typesValid = left->type == U64 && right->type == U64`.
   - **VM Layer** (`src/rules/dsl_vm.cpp:665-710`):
     - In `evaluateTypedExpression`, convert `Bool` scalar values to `1ULL` (true) or `0ULL` (false) before executing integer addition and multiplication.

## Consequences

### Positive
- Directly enables pure functions and computed fields to express standard indicator-based piecewise mappings (such as ITU-T H.264 Table D-1).
- Preserves complete unsigned arithmetic safety by disallowing boolean operands in subtraction and division.
- Fully backwards compatible with existing `.svfmt` rules.

### Negative / Trade-offs
- Introduces implicit numeric promotion from `Bool` to `U64` in additive and multiplicative contexts.

### Capability Verification Matrix
- Pure function evaluating ITU-T H.264 Table D-1 across all 16 `pic_struct` values ($0 \le \text{pic\_struct} \le 15$):
  - Values 0, 1, 2 $\to$ 1
  - Values 3, 4, 7 $\to$ 2
  - Values 5, 6, 8 $\to$ 3
  - Values 9..15 $\to$ 0
- Unit tests covering `bool + bool`, `bool * bool`, `bool * u64`, `u64 * bool`, `bool + u64`, `u64 + bool`.
- Negative tests asserting that `bool - u64`, `u64 - bool`, `bool / u64`, `u64 / bool`, `bool % u64` are rejected at compile time.
