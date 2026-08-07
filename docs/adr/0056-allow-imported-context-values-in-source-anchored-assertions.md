# Allow Imported Context Values in Source-Anchored Assertions

Status: Accepted
Date: 2026-08-07

## Context

ADR-0054 added source-anchored assertions whose conditions can relate local
decoded and computed scalars. ADR-0046 and ADR-0052 separately expose one
scalar from an exact imported context generation to dynamic bit widths and a
restricted equality guard. Those forms still cannot express a top-level
conformance prerequisite that is conditional on local syntax and compares a
PPS or SPS export.

The next bounded H.264 non-IDR P-slice increment must omit
`pred_weight_table()` and `cabac_init_idc` only when the selected PPS disables
weighted prediction and entropy coding. Assertions remain unconditional
top-level statements, so the rule needs a short-circuit condition such as
`!is_p_slice || context_value(...) == 0` before it can safely decode the next
field. Moving these checks into the C++ analyzer would violate the rule-owned
format boundary.

## Decision

Allow the reserved expression leaf
`context_value(import_key, context_kind, exported_field)` inside the Boolean
condition of a source-anchored assertion:

```cpp
assert(!is_p_slice ||
       context_value(pic_parameter_set_id,
                     h264_pps,
                     weighted_pred_flag) == 0)
    at pic_parameter_set_id;
```

The call has exactly three identifier arguments and has static type `u64`.
The import key, reachable target kind, unique publishing structure, and named
export use the exact static resolution contract from ADR-0046. The leaf may
participate in the existing checked arithmetic, comparison, Boolean, and
pure-call expression contract for that assertion. It is not implicitly
convertible to `bool`; the complete assertion condition must still have type
`bool`. Existing depth, node, pure-inlining, and expansion-work limits apply.

Parser validation recognizes this reserved call only while validating an
assertion condition. Pure-function bodies, computed fields, lazy byte counts,
and other general expression positions continue to reject a directly written
`context_value`. A pure function may receive the imported `u64` as an argument
from an assertion, because the call is statically inlined and does not acquire
context access of its own.

The compiler enables the existing exact imported-context resolver only while
lowering the assertion condition. The result is the existing canonical
`ImportedContextReference` typed-expression leaf containing the import ordinal,
target definition kind, publishing structure index, and export ordinal. The
assertion descriptor, statement position, source anchor, and
`AssertExpression` bytecode instruction do not change.

Before reading source, the VM validates imported-reference descriptors in
assertion conditions with the same publisher, export, reachability, type, and
operand checks used by other imported expressions. At execution, the existing
context-value resolver selects and caches the exact generation closure. Boolean
short-circuiting remains observable: an imported leaf in an unselected operand
is not resolved.

If the imported value resolves and the complete condition is false, execution
returns fatal `invalid-syntax` with message `Assertion condition is false` and
uses the assertion's `at` field as the diagnostic path and mapped source range.
If context resolution itself fails, the resolver's existing status and message
are preserved and the diagnostic points to the materialized import-key field.
Missing, future, or stale generations therefore remain
`dependency-unavailable`; malformed context payloads or descriptors remain
invalid definitions. Successful assertions still read no source bits, move no
cursor, and create no presentation node.

Regression coverage includes the three-identifier parser contract, rejection
outside assertions, typed imported descriptors and positioned bytecode,
malformed descriptor preflight before source access, passing and failing exact
generations, short-circuit behavior, and missing-generation diagnostics.

## Consequences

Rules can state source-located conformance prerequisites that combine local
syntax with exact imported parameter-set values while the analyzer remains
format-neutral. The extension reuses the existing bounded expression,
context-generation, typed-IR, bytecode, and runtime resolver models rather than
adding a second assertion or context mechanism.

The diagnostic distinction remains explicit: a violated conformance rule is
anchored where the rule author requested, while an unavailable dependency is
anchored where the consumer encoded its import key.

## Non-goals

This decision does not allow imported values in pure-function bodies, computed
fields, lazy byte counts, switch or repeat controllers, repeat bounds, sentinel
termination, payload dispatch, annotations, array lengths, or general
conditional expressions beyond ADR-0052. It does not add implicit imported
Boolean values, fallback generation selection, conditional or repeated
assertions, warning assertions, custom messages, or new context kinds.

It does not itself add an H.264 P-slice, weighted-prediction table,
`cabac_init_idc`, reference-list modification loop, reference-picture marking,
or entropy-coded slice-data decoding.
