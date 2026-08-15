# Locally Scoped Context Import Keys

Status: Accepted
Date: 2026-08-15

## Context

ITU-T H.264 specifies several Supplemental Enhancement Information (SEI) payload types
that depend on the active Sequence Parameter Set (SPS). For instance, clause D.1.1 and
clause D.2.1 specify the Buffering Period SEI message (`payload_type == 0`):

```text
buffering_period( payloadSize ) {
    seq_parameter_set_id                            ue(v)
    if( !HrdParamPresentFlag )
        // no HRD fields present
    if( NalHrdBpPresentFlag ) {
        for( SchedSelIdx = 0; SchedSelIdx <= cpb_cnt_minus1; SchedSelIdx++ ) {
            initial_cpb_removal_delay[ SchedSelIdx ]          u(v)
            initial_cpb_removal_delay_offset[ SchedSelIdx ]   u(v)
        }
    }
    if( VclHrdBpPresentFlag ) {
        for( SchedSelIdx = 0; SchedSelIdx <= cpb_cnt_minus1; SchedSelIdx++ ) {
            initial_cpb_removal_delay[ SchedSelIdx ]          u(v)
            initial_cpb_removal_delay_offset[ SchedSelIdx ]   u(v)
        }
    }
}
```

In `buffering_period`, `seq_parameter_set_id` is parsed inside the SEI message payload,
and its value is used to query the active SPS for HRD parameters (`nal_hrd_parameters_present_flag`,
`nal_hrd_cpb_cnt_minus1`, `nal_hrd_initial_cpb_removal_delay_length_minus1`, etc.) to drive:
1. Loop iteration bounds (`repeat(cpb_cnt_minus1 + 1)`), and
2. Dynamic bit widths (`bits<(initial_cpb_removal_delay_length_minus1 + 1)>`).

### Probing and Language Limitation

When attempting to express this in `SeiRbsp`:
```svfmt
@context_import("h264-sps", seq_parameter_set_id)
struct SeiRbsp {
    repeat (64) while (more_rbsp_data()) {
        ff_coded<8> payload_type;
        ff_coded<64> payload_size;
        switch (payload_type) {
            case 0: {
                ue seq_parameter_set_id;
                computed<u64> imported_nal_hrd_present =
                    context_value(seq_parameter_set_id, h264_sps, nal_hrd_parameters_present_flag);
            }
            default: {
                @lazy(payload_size) bytes payload_data;
            }
        }
    }
    rbsp_trailing_bits;
}
```

Running `svtool rule check` produced the following compiler errors:
```text
probe_t10_import.svfmt:8:1: error: Context import key field is not a top-level scalar field
probe_t10_import.svfmt:17:35: error: context_value import key must be an earlier context-eligible field
```

Investigation revealed two enforcement gates in the DSL IR compiler (`src/rules/dsl_ir.cpp`):
1. **Annotation Gate (`dsl_ir.cpp:3101-3134`)**: Enforced that the key field declared in `@context_import("...", keyFieldName)` must be a top-level, unconditionally declared scalar field of the struct.
2. **Expression Gate (`dsl_ir.cpp:1527-1537`)**: Enforced that the `keyName` argument in `context_value(keyName, ...)` must resolve to a top-level declared field rather than a branch-guaranteed local field.

Furthermore, nested struct instantiation inside control flow branches is not part of the DSL syntax (`Expected bits<...>, ue, se, or ff_coded field type`), and custom SEI passthrough mechanisms would violate core format-neutrality invariants.

## Decision

We introduce **Locally Scoped Context Import Keys** by relaxing the positional constraint on context import keys while preserving all typing and scoping invariants:

1. **Declaration Syntax Unchanged**:
   The syntax `@context_import("h264-sps", seq_parameter_set_id)` remains at the struct level.
   
2. **Relaxation of Import Key Gates**:
   - The annotation gate (`dsl_ir.cpp`) allows import key fields to be declared inside control flow branches (including `repeat` loops and `switch`/`if` blocks).
   - In contrast, `@context` definition keys and `@context_dependency` retain their top-level unconditional requirements to ensure deterministic directory registration.
   
3. **Branch-Guaranteed Static Binding**:
   - Each `context_value(keyName, contextKind, fieldName)` expression statically resolves `keyName` to the nearest earlier declaration that is guaranteed on the current execution path, reusing the established branch-guarantee analysis (`dsl_ir.cpp:1443-1497`).
   - If `keyName` is not declared on the current path, or is declared in a disjoint/non-dominating conditional branch, compilation fails with `DslDiagnosticCode::InvalidContext`.

4. **Key Type Invariants**:
   - The key must be an unsigned scalar: `bits<N>` (unsigned), `ue`, `ff_coded<N>`, or `computed<u64>`.
   - Signed fields (`se`) and arrays are strictly rejected as context keys.

5. **Per-Iteration Rebinding**:
   - In `repeat` loops (such as the SEI message parsing loop), each unrolled iteration binds to its own local key slot. Subsequent expressions within the same iteration evaluate against the SPS generation selected by that iteration's key.

6. **Fault Isolation and Partial Failure**:
   - If a local context key is unavailable (e.g. invalid SPS ID or missing generation), only the current message transitions to `waiting-dependency` or `invalid`. The SEI container continues processing subsequent messages according to `payload_size`.

7. **Scope Limitation (Non-Goal)**:
   - This capability strictly requires an explicit, locally parsed key field. Payload types that lack an explicit key (such as `pic_timing`) cannot rely on locally scoped import keys and will require independent probing.

## Consequences

- SEI payloads such as `buffering_period` (`payload_type == 0`) can parse `ue seq_parameter_set_id` inside loop and switch branches, and query SPS HRD context parameters directly.
- Context directory lookup, generation history selection by source offset, and caching mechanics remain entirely unchanged.
- No new parser grammar or dynamic runtime scoping is added; binding remains statically determined at IR compilation time.

## Follow-up

- ADR-0028: Context Directory and SPS/PPS Lifecycle
- ADR-0078: Select Context Generations by Stream Position Across Parameter Set Redefinitions
- ADR-0080: Bounded Iteration over RBSP Data with While-Repeat
- ADR-0081: Decode the Recovery Point SEI Message
- ADR-0082: Decode the User Data Unregistered SEI Message
- ADR-0083: Decode the User Data Registered ITU-T T.35 SEI Message
