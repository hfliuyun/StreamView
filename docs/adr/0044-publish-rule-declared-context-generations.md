# Publish Rule-Declared Context Generations

Status: Accepted
Date: 2026-08-03

## Context

The bundled H.264 rule can now materialize bounded SPS and PPS structures, but
those structures do not yet establish the position-aware generations required
by slice headers. ADR-0028 deliberately left session and runner plumbing until
the first consuming format rule. The existing `ContextDirectory` already owns
the format-neutral source-position and exact-generation policy; typed SPS and
PPS values must remain in the rules layer.

The current `DslExecutor` is a stateless pass-through to the VM. The Annex B
runner separately creates readers, enforces exact RBSP consumption, and obtains
runtime values by walking presentation-tree children by field name. Adding
context registration to that path would spread publication ordering, typed
payload ownership, and dependency failure recovery across the analyzer and
future AAC and ISO BMFF callers.

Slice-header decoding will also need rule-declared context imports, dynamic bit
widths, bounded sentinel loops, and an explicit compressed-payload terminal.
Those later language additions require a stable execution seam, but they do not
all need to be implemented before SPS and PPS generations become real.

## Decision

Add a rules-layer deep module named `RuleExecutionSession`. One instance belongs
to one analysis source and one exact compiled rule. Its `run` interface executes
one structure over a supplied logical view, enforces the requested exact
consumption policy, stages rule effects, and publishes them only after the
structure has materialized successfully. The module owns one format-neutral
`ContextDirectory` and a private typed-payload association keyed by returned
`ContextDefinitionId` values. The Annex B runner supplies the source, mapping,
enclosing source span, parent analysis node, and cancellation options; it does
not interpret parameter-set fields or call the directory itself.

The format definition language adds three annotations for the first bounded
publication slice:

```cpp
@context("h264-sps", seq_parameter_set_id)
struct SequenceParameterSetRbsp {
    ue seq_parameter_set_id;
    ue log2_max_frame_num_minus4 @context_export;
    // ...
}

@context("h264-pps", pic_parameter_set_id)
@context_dependency("h264-sps", seq_parameter_set_id)
struct PictureParameterSetRbsp {
    ue pic_parameter_set_id;
    ue seq_parameter_set_id;
    bits<1> entropy_coding_mode_flag @context_export;
    // ...
}
```

`@context` occurs at most once on a structure and requires one recognized
directory-kind string plus one field-name argument. The recognized strings map
only to the closed core kinds `h264-sps`, `h264-pps`, `aac-asc`, and
`iso-bmff-sample-description`. `@context_dependency` may repeat at most 16 times
and has the same argument shape. It is valid only when the structure also has
`@context`. `@context_export` takes no arguments and selects at most 64 values
for the private typed payload.
An identical dependency kind and field pair is a static duplicate error rather
than an idempotent declaration.

Every key, dependency key, and exported value must be an unconditional,
top-level, non-array unsigned scalar declared in the same structure. The first
slice accepts `bits`, enum, `ue`, and unsigned computed fields. A field inside a
conditional, switch, repeat, or fixed array is rejected because publication
must never depend on a value absent from a successfully selected path. Signed
values, lazy regions, generated trailing-bit fields, and presentation-only
regions are not context values.

The compiler resolves all annotation field names to stable typed-IR indexes.
The VM returns values only for those resolved key, dependency, and export
indexes; it does not expose its complete structure-local environment and the
runtime never reads values back from the analysis tree. The private payload
records the publishing structure schema and explicitly exported scalar values.
It is prepared and capacity is reserved before directory mutation. After a
successful directory registration, committing the prepared payload is a
non-allocating move, so the directory and rules-owned association become
visible together under the existing single-writer model.

A definition uses the complete enclosing NAL-unit source span as its
availability span and the materialized SPS or PPS structure node as its stable
analysis node. Standalone Annex B uses scope zero. A PPS dependency is resolved
at the PPS span start; the returned SPS definition ID is registered as an exact
generation dependency. Missing, future, stale, cyclic, or otherwise unavailable
dependencies never fall back. The PPS syntax structure remains materialized,
but receives a source-located `dependency-unavailable` diagnostic, its RBSP and
NAL are invalid for this run, no PPS generation or typed payload is published,
and scanning continues with later NAL units.

Malformed, truncated, cancelled, resource-limited, not-exactly-consumed, or
otherwise invalid structures publish no context. A later malformed redefinition
does not hide the preceding valid generation, matching ADR-0028. Context
registration metadata rejection is an invalid rule/runtime state rather than a
media-syntax fallback.

The first valid `run` locks the session to one source and one analysis-tree
instance. Reuse with a different source or tree is invalid rule/runtime state;
moving the session or containing analyzer preserves that identity. A nonzero
logical start executes the mapped suffix beginning there, preserves the mapping's
logical coordinates, and applies exact consumption to that suffix. Every source
span mapped by a context execution suffix must be contained by its declared
non-empty enclosing source span; a mismatch is rejected before reads or binding.

The bundled SPS and PPS declarations adopt these annotations and publish the
unsigned scalar values required by the planned bounded slice-header rule. The
package coverage token remains `parameter-sets`; the rule source is published
as package version `0.1.7`.

## Consequences

SPS and PPS materialization now establishes real source-position generations.
A PPS binds the exact SPS generation current before its own NAL and becomes
unavailable after that dependency is superseded, without teaching the Annex B
analyzer H.264 field names or parameter-set semantics.

The execution session becomes the interface tested by context publication and
later imports. The existing VM remains responsible for deterministic bounded
field execution, while the session owns cross-execution state, exact-consumption
policy, staged effects, and diagnostic translation. Callers without context
annotations continue through the same `run` path with no directory effect.

Future slice-header work adds context-import typed IR, dynamic `bits<expression>`,
bounded sentinel loops, and a final compressed remaining-bit region behind this
same module. Imported values will come from the rules-owned payload associated
with the exact generation selected by `ContextDirectory`; they will not be read
from presentation nodes or implemented as H.264 analyzer callbacks.

## Non-goals

This decision does not yet dispatch or decode slice NAL units, import PPS/SPS
values into a VM frame, add dynamic-width syntax, add sentinel-terminated loops,
or preserve slice data as a compressed payload. It does not persist live context
state or typed payloads in SQLite or saved sessions, assign nonzero container
track scopes, register malformed parameter sets, or open the core directory-kind
enum to arbitrary installed rules.
