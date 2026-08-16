# ADR-0098: Unrecognized Annotation Compiler Gate and Explicit Unsupported Syntax

- **Status**: Accepted
- **Date**: 2026-08-17
- **Authors**: StreamView Contributors

---

## Context and Motivation

During format coverage expansion and security auditing of the DSL compiler and runtime, two distinct language-level capabilities were identified as essential:

1. **Unrecognized Annotation Silent Bypass (N2 Vulnerability)**:
   The DSL compiler previously checked recognized annotations (`@equals`, `@range`, `@enum`, `@spec`, `@description`, `@context_export`) on declared fields, but silently ignored unknown or misspelled annotations. For example, `bits<12> syncword @equalss(4095);` compiled cleanly with `Rule OK`, and executing against an invalid stream yielded `Materialized` with 0 diagnostics, silently omitting the constraint.
2. **Explicit Non-Fatal Unsupported Feature Reporting**:
   StreamView format rules operate within bounded profile subsets (e.g. H.264 `baseline`/`main`/`high`, AAC `lc`). When a bitstream requests a valid but unsupported specification feature (such as non-GA Audio Object Types like SBR/AOT 5, PS/AOT 29, or escaped AOT >= 31 in AAC, or unhandled extensions in MP4 containers), the analyzer must:
   - Halt further decoding of the unsupported syntax subtree;
   - Preserve already materialized header fields with valid coordinates;
   - Publish the node with `MaterializationState::Unsupported` and a non-fatal `DiagnosticCode::UnsupportedSyntax` warning;
   - Maintain format-neutral core engine decoupling without format-specific C++ string synthesis (Gemini Rule 5.1).

---

## Decision

### 1. Unified Compile-Time Annotation Registry and Gate

A table-driven static registry (`knownAnnotations`) and unified validation function (`validateAnnotations`) are established in [`src/rules/dsl.cpp`](file:///Users/yun/code/streamview/src/rules/dsl.cpp):

1. **Active Recognized Annotations (11)**:
   - `@spec("standard", "clause")`: Bit fields, computed fields, lazy byte regions, compressed payloads, structures, enums, scan sequences, payload dispatches.
   - `@description("text")`: Bit fields, computed fields, lazy byte regions, compressed payloads, structures, enums, scan sequences, payload dispatches.
   - `@equals(integer)`: Bit fields (`bits`, `ue`).
   - `@range(minimum, maximum)`: Bit fields (`bits`, `ue`, `se`).
   - `@enum(EnumType)`: Bit fields (`bits`, `ue`).
   - `@lazy(expression)`: Dedicated field introducer (`@lazy(expr) bytes name;`), not a general annotation.
   - `@index(progressive)`: Sequence scan declarations (`sequence<T> s = scan(scanner);`).
   - `@context("kind", key)`: Structure declarations.
   - `@context_export`: Bit fields and computed fields.
   - `@context_import("kind"[, key])`: Structure declarations.
   - `@context_dependency("kind", key)`: Structure declarations.
2. **Reserved Annotations (1)**:
   - `@target_format(...)`: Reserved for future cross-layer format delegation (Task P5h), currently registered with `allowedTargets = 0` (rejected on all hosts).
3. **Compile-Time Gate**:
   - Any unregistered annotation name triggers `DslDiagnosticCode::InvalidAnnotation` (`"Unknown annotation '@<name>'"`).
   - Annotations placed on unsupported declaration hosts trigger `DslDiagnosticCode::InvalidAnnotation` with host-specific whitelist diagnostics.
   - Pure functions (`pure`) and entry declarations (`entry`) strictly reject all annotations.

### 2. Explicit `unsupported` Syntax Statement

A format-neutral declaration statement is introduced into the DSL grammar:

```svfmt
unsupported("reason text") at anchor_field;
```

1. **Parser & Compiler Rules**:
   - The anchor field must be a source-backed scalar field declared earlier on the current execution branch.
   - Prohibited inside `repeat` loops (`DslDiagnosticCode::InvalidCondition: "Unsupported statements cannot be repeat-local items"`). Diagnostics emitted across unrolled loop bodies are deduplicated by `(code, message, range)` in `addDiagnostic` ([`src/rules/dsl_ir.cpp:28-36`](file:///Users/yun/code/streamview/src/rules/dsl_ir.cpp#L28-L36)).
   - A single structure may declare at most 1024 unsupported statements.
2. **Bytecode & VM Execution**:
   - Lowered to opcode `DslOpcode::MarkUnsupported`.
   - When reached, the VM ceases further decoding of the structure, sets `DslExecutionStatus::Unsupported`, sets node state to `core::MaterializationState::Unsupported`, and emits `core::DiagnosticCode::UnsupportedSyntax` (Severity: Warning) anchored at the specified field.
3. **AAC Profile Application**:
   - In the official `org.streamview.aac` package (`profiles = ["lc"]`), non-GA AOTs (AOT 5, 29) and escaped AOT configurations (`audio_object_type == 31`) immediately halt after their common prefix and emit `unsupported`. All escaped AOT values (AOT 32, 34, 40, 41, 42, etc.) are uniformly classified as Unsupported without per-value branch decoding.

---

## Consequences

### Positive
- Misspelled annotations can no longer silently bypass validation or omit critical format constraints.
- Format rules express profile boundaries and unsupported extensions natively within the DSL rather than relying on C++ analyzer hacks.
- Diagnostics during compilation are deduplicated across unrolled loop iterations.

### Negative / Limitations
- Adding new annotations in the future requires updating the central `knownAnnotations` registry table.

---

## Rejected Alternatives

1. **Decentralized Ad-Hoc If-Checks**:
   Scattering annotation checks across separate parser functions was rejected due to risk of divergence, omission on new declaration hosts, and maintenance overhead.
2. **Fatal `InvalidSyntax` on Unsupported Profiles**:
   Rejecting unsupported profile streams with fatal errors was rejected because unsupported standard profiles are valid media bitstreams; the analyzer must present decoded header metadata non-fatally.
3. **Format-Specific C++ Diagnostic Synthesis**:
   Synthesizing unsupported warnings in C++ analyzers was rejected to preserve format-neutral core separation (Gemini Rule 5.1).

---

## Verification Matrix and Evidence

| Probe / Test Case | Command / Test Symbol | Expected Output / Assertion | Verification Result |
| :--- | :--- | :--- | :--- |
| **Unknown Annotation Gate (N2)** | `scratch/probe_annotation_gate` | `diag code=14 msg="Unknown annotation '@equalss'"` | Verified compile error on misspelled annotation. |
| **Host Whitelist Validation** | `tests/rules/dsl_test.cpp:2171` (`rejectsUnrecognizedAnnotationsAndEnforcesHostWhitelist`) | `InvalidAnnotation` on all 5 host positions (`@bogus(1)`) | Verified on leading/trailing field, struct, enum, sequence. |
| **Field Error Recovery Guard** | `tests/rules/dsl_test.cpp:2242` (`recoversFieldSyntaxErrorWithoutDroppingClosingBrace`) | Exactly 1 diagnostic (`MissingToken: Expected field name`) | Verified struct closing brace preserved. |
| **Unsupported Loop Deduplication** | `tests/rules/dsl_ir_test.cpp:3562` (`deduplicatesUnsupportedDiagnosticsInsideRepeats`) | Exactly 1 diagnostic (`Unsupported statements cannot be repeat-local items`) | Verified loop diagnostic deduplication. |
| **AAC Non-GA AOT Unsupported** | `tests/rules/aac_adts_analyzer_test.cpp:1960-2467` | `MaterializationState::Unsupported`, `UnsupportedSyntax` | Verified AOT 5, 29, 39, and escaped AOT 31. |
| **Bundled Rules Verification** | `svtool rule check` on H.264 & AAC packages | `Rule OK` across all `.svfmt` files | Zero regressions across official rules. |

---

## References

- [ADR-0040: Non-Fatal Syntax Warnings and Range Annotations](0040-non-fatal-syntax-warnings-and-range-annotations.md)
- [ADR-0094: Audio Specific Config and Program Config Element](0094-audio-specific-config-and-program-config-element.md)
- [ADR-0095: AAC Raw Data Block Compressed Payload and Profile Handling](0095-aac-raw-data-block-compressed-payload-and-profile-handling.md)
- [ADR-0096: MP4 ISOBMFF Container Architecture, Box Traversal, and Cross-Layer Navigation](0096-mp4-isobmff-container-architecture-box-traversal-and-cross-layer-navigation.md)
