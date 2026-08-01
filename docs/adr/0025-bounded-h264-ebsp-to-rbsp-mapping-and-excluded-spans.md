# Bound H.264 EBSP-to-RBSP Mapping and Expose Excluded Spans

Status: Accepted
Date: 2026-07-28

## Context

ADR-0024 lets a `BitReader` consume a source-mapped logical view without
copying transformed bytes. The remaining H.264 path still has no component
that builds the RBSP mapping or classifies bytes excluded from it. The current
Annex B analyzer therefore decodes only the direct one-byte NAL header and
leaves every later payload byte uninterpreted.

ITU-T H.264 (08/2024) clause 7.3.1 parses the NAL-unit header before producing
RBSP bytes. Starting after the complete header, each complete source sequence
`00 00 03` contributes the two zero bytes to the RBSP and discards the `03` as
an `emulation_prevention_three_byte`. This decoding rule does not depend on a
fourth byte. In particular, a terminal `00 00 03` still excludes the `03`, and
`00 00 03 04` still excludes the `03` even though clause 7.4.1 prohibits that
four-byte sequence.

Clause 7.4.1 separately constrains conforming NAL-unit contents. It prohibits
the byte-aligned sequences `00 00 00`, `00 00 01`, and `00 00 02`, prohibits a
four-byte sequence beginning `00 00 03` when its fourth byte is greater than
`03`, and prohibits a final NAL-unit byte equal to `00`. These validity rules
must produce diagnostics without changing the clause 7.3.1 transformation or
silently preserving a byte that the decoder must discard.

A NAL unit may be very large, and adversarial input can alternate forwarded and
excluded bytes often enough to create storage proportional to input size. The
mapper must therefore preserve exact source coordinates while bounding work,
mapping segments, excluded-span records, diagnostics, and cancellation
latency.

## Decision

The rules module will provide a reusable stateful H.264 EBSP-to-RBSP mapper. It
will operate on a `RandomAccessSource`, a caller-owned RBSP logical-view ID, and
one exact finite `SourceSpan` containing only the EBSP bytes after the complete
NAL-unit header. The span must be byte-aligned, have a byte-multiple length, fit
the source, and fit the bit-coordinate domain. The mapper will not infer Annex
B boundaries or header length. Annex B and future AVC-in-ISO-BMFF callers must
determine those boundaries first.

For Annex B input, the caller must first apply the byte-stream extraction in
clauses B.1 and B.2. `leading_zero_8bits`, `zero_byte`,
`start_code_prefix_one_3bytes`, and `trailing_zero_8bits` are framing and are
not part of the EBSP span passed to the mapper. The Annex B scanner will split
the maximal zero-byte run before the next start code or end of source from the
preceding `nalUnit` span and expose that run separately. The analyzer will keep
it visible as one materialized `trailing_zero_8bits` region covering the run.

The basic one-byte AVC NAL header remains direct source data. NAL-unit types
with extension headers are not passed to the mapper until the caller has parsed
and excluded their complete header. The initial Annex B integration will keep
such out-of-scope payloads uninterpreted rather than treating extension-header
bytes as RBSP.

The mapper will inspect source bytes once in increasing order and retain only
bounded scan state. When the scan cursor reaches a complete source sequence
`00 00 03`, the two zeros are forwarded and the `03` is excluded; no following
byte is required. After an exclusion, the zero-run state is reset so a later
source byte is not matched across the discarded byte. Adjacent forwarded bytes
are coalesced into one source span. Each excluded record contains its exact
eight-bit source span and the RBSP logical bit offset at which the gap occurs.

Transformation and conformance checking are distinct outputs of the same
scan:

- `00 00 03` excludes `03`, including at the end of the input.
- `00 00 03 xx` with `xx > 03` still excludes `03` and also reports the
  prohibited four-byte sequence.
- `00 00 00`, `00 00 01`, and `00 00 02` are forwarded as required by the
  transformation and reported as prohibited three-byte sequences. Overlapping
  prohibited sequences are reported at their byte-aligned source positions.
- A non-empty input whose final source byte is `00` reports the final-byte
  violation.

These checks cover sequences wholly contained in the mapper input. Header
semantics and any conformance condition crossing the header/payload boundary
remain responsibilities of the caller that determined the header length.

Conformance issues carry exact source spans and do not make a successfully
constructed mapping unavailable. The Annex B analyzer marks the affected NAL
region invalid, retains the complete RBSP mapping and excluded nodes, and
continues scanning later NAL units. RBSP syntax and `rbsp_trailing_bits()`
validation remain later parsing responsibilities. The NAL region remains
`Indexing` while its payload, excluded, framing, and diagnostic children are
appended; only then is it transitioned to `Invalid`.

The mapper exposes cumulative forwarded spans as a normalized `SourceMapping`,
cumulative excluded records, cumulative conformance issues, its source cursor,
and its RBSP logical length. Its batch result distinguishes `InProgress`,
`Complete`, `Cancelled`, `SourceError`, `InvalidInput`, `InvalidBatchSize`, and
`ResourceLimit`. A complete result may still contain conformance issues.

Each mapping batch accepts a positive maximum number of source bytes to inspect;
the default is 64 KiB. Cancellation is observed before work and at least every
1,024 inspected bytes. Construction also receives positive cumulative limits
for mapping segments, excluded records, and conformance issues; the initial
defaults are 65,536 mapping segments, 65,536 excluded records, and 1,024
issues. Reaching an output limit stops before committing the item that would
exceed it and returns `ResourceLimit`. Zero batch or cumulative limits are
invalid and do not consume source data.

`InProgress` and terminal failures retain a valid mapping for the fully
committed prefix. A cancellation does not consume the next uninspected byte. A
source error, end-of-source inside the validated input span, or inconsistent
successful read returns `SourceError` with the source message when available.
Repeated calls after `Complete`, `Cancelled`, `SourceError`, or `ResourceLimit`
replay the terminal status without new output. No transformed byte buffer is
allocated. Mapper `InvalidInput` indicates a caller contract violation; the
trusted Annex B integration translates that otherwise unreachable state to its
existing `InvalidRule` terminal status rather than treating malformed media as
an invalid mapping argument.

The Annex B analyzer will run mapping as a separate bounded stage and will not
scan an entire large NAL payload inside one analysis batch. Completed earlier
NAL regions remain published while mapping is in progress. A mapping failure
retains the direct NAL header and the mapper's committed prefix, marks the
current NAL and RBSP payload with the corresponding state and diagnostic, and
preserves the existing root-level source-error, resource-limit, and
cancellation behavior.

After a successfully decoded direct header, a non-empty mapped payload is
presented under the existing `nal_unit[index]` region as follows:

- A successful `rbsp_payload` is a materialized `Region` whose location covers
  the complete RBSP logical range and therefore contains only forwarded source
  spans. Its metadata identifies the H.264 RBSP logical view and clauses 7.3.1
  and 7.4.1.
- A mapper failure publishes `rbsp_payload` with the committed-prefix location,
  if non-empty, and the matching `Cancelled` or `Invalid` state and diagnostic.
- `emulation_prevention_three_byte[index]` is a materialized `Region` for each
  excluded byte, indexed in source order and located in the direct EBSP view.
- New nodes are appended after the existing `start_code` and `NalUnitHeader`
  children and before the optional `trailing_zero_8bits` framing region. An
  empty final NAL, a failed header, and a header-only NAL add no mapped payload
  or excluded node; independently identified trailing-zero framing may still be
  present.

All payload and framing children are appended while their NAL parent remains
`Indexing`. The parent is transitioned only after those children and their
diagnostics have been committed, because terminal analysis nodes do not accept
later children.

An excluded region is not a syntax field, has no RBSP logical coordinate, and
is never absorbed into a field or diagnostic resolved through the RBSP
mapping. Selecting a future RBSP field that crosses an excluded byte highlights
only its disjoint forwarded spans; selecting the excluded byte resolves to its
named region. `Region` is used instead of `CompressedPayload` because an RBSP
may contain ordinary syntax structures as well as opaque slice data.

This slice does not accept the provisional general-purpose `view`, `forward`,
`skip`, or `slice` DSL syntax. It also does not define RBSP trailing-bit syntax,
SPS/PPS or slice parsing, extension-header parsing, lazy payload boundaries, or
progressive persistence.

## Amendment (2026-08-01)

ADR-0038 adds the bounded `rbsp_trailing_bits;` DSL terminal and moves that
specific deferred responsibility into formal RBSP parsing. This ADR continues
to own only the EBSP-to-RBSP mapping and excluded-span contract; it does not
add a general view, forwarding, slicing, or alignment language feature.

## Consequences

H.264 syntax can consume a logical RBSP directly from the unchanged source and
cross any number of bounded emulation-prevention gaps while retaining exact
multi-span locations. Excluded bytes remain individually visible and
selectable in the analysis tree and raw-data view.

Malformed EBSP is neither silently normalized into a conforming stream nor
made unreadable solely because of a conformance issue. The decoder mapping
follows clause 7.3.1, while clause 7.4.1 violations remain source-located and
visible on the affected NAL region.

Mapping storage is proportional to the number of forwarded runs plus retained
excluded and issue records, each explicitly capped, rather than the number of
forwarded bytes. Per-batch work and cancellation latency are bounded, and the
cumulative caps prevent adversarial inputs from creating an unbounded mapping
or analysis subtree. A caller that needs more payload must resume bounded
batches or, after the later lazy-boundary slice, materialize only the requested
region.

The helper is H.264-specific but container-independent. Annex B and future MP4
sample navigation share one transformation contract without putting codec
escape rules in the core reader or duplicating decoded payload bytes.

## Considered Options

- Exclude `03` only when a following byte is at most `03`: this confuses the
  conformance restriction with the clause 7.3.1 decoder transformation and
  would map malformed `00 00 03 04` input incorrectly.
- Copy RBSP bytes into a decoded buffer: straightforward to parse, but it
  duplicates large payloads and requires a second coordinate authority.
- Put emulation-prevention handling in `BitReader`: convenient for H.264, but it
  would make a codec rule part of the general core read contract.
- Hide excluded bytes in mapping metadata: compact, but raw-data selection
  could not resolve an excluded byte to a named structural role.
- Add general mapped-view DSL syntax now: eventually useful, but it expands the
  language, compiler, and VM before the verified H.264 behavior and limits are
  established.
