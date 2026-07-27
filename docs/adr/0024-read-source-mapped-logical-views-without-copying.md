# Read Source-Mapped Logical Views Without Copying

Status: Accepted
Date: 2026-07-28

## Context

`SourceMapping` already represents one logical view as an ordered sequence of
absolute source spans, and `FieldLocation` already resolves a logical field to
one or more disjoint spans. The analysis tree, field inspector, raw-data
highlighting, and source-bit lookup all preserve those multi-span locations.

The remaining execution path is still direct-only. `BitReader` reads one
contiguous `SourceSpan`, while the DSL executor receives a separate mapping and
rejects any field whose location is not exactly one contiguous span. Copying a
transformed view into a decoded buffer would make reads easy but would lose the
read-only source relationship, duplicate potentially large media data, and
create another coordinate authority.

M4 needs a general mapped read before the H.264 EBSP-to-RBSP helper can omit an
emulation-prevention byte and still let a field cross that gap with an exact
location.

## Decision

`BitReader` will support an ordered source-mapped backing in addition to its
existing contiguous-range construction. Its public position, remaining length,
seek offsets, and read bounds are logical offsets within that backing. A mapped
reader forwards only the bits named by its ordered source spans; a source gap is
never read and contributes no logical bits.

The reader exposes its logical length and normalized ordered backing spans as
read-only execution metadata. `SourceMapping` remains relative to the active
analysis source and does not acquire a separate source identity in this slice.
The analysis-session owner must construct the reader and mapping from the same
`RandomAccessSource`; source identity and fingerprint validation remain session
responsibilities.

A mapped reader is created only from a valid `SourceMapping` or a valid logical
slice resolved through that mapping. An empty mapping creates a zero-length
reader. The implementation may normalize or index mapping segments, but it does
not materialize a decoded copy of the complete logical view. The existing
contiguous constructor retains its behavior and is treated as a one-span
logical backing.

`readBits` keeps its current `1..64` contract and most-significant-bit-first
value order. One read may split at source-span boundaries. Work and temporary
storage are bounded by the requested bit count and the mapping segments crossed
by those bits, not by the complete view length. A read past the logical bound
returns `EndOfRange`; unavailable source data returns `EndOfSource`; an I/O or
inconsistent successful source read returns `SourceError`. Every failure leaves
the reader position unchanged, including a failure after an earlier segment of
the same requested value was read successfully. `seek` changes only the logical
position and rejects an offset beyond the logical length.

The DSL execution boundary will validate, before bytecode execution, that the
reader's complete normalized backing is exactly the source resolution of the
mapping range beginning at `logicalStart` and having the reader's logical
length. An out-of-range mapping slice, reordered span, missing span, additional
span, or different source length is an invalid typed execution. Within the
active analysis source, this prevents a caller from
decoding one range while publishing a location for another; it does not replace
the session's same-source ownership check.

Field reads continue to advance through logical positions. After a successful
read, the VM obtains the field location from the execution mapping. The location
may contain one or more source spans; the direct single-span restriction is
removed. Diagnostics use the same logical-to-source resolution and therefore
include only available forwarded spans, never an excluded gap.

Little-endian fields still reverse complete logical-byte significance after the
reader has assembled the value in logical MSB-first order. Their declared width
and structure-relative start must remain byte-aligned as before. At execution,
the field's logical start and the first resolved source bit must also be byte-
aligned. Later mapping-segment boundaries need not be source-byte-aligned:
logical bytes, rather than source segmentation, define the units whose value
significance is reversed. A source gap does not itself invalidate the field or
change its location.

Mapped reads add no bytecode opcode, analysis node, source mutation, or decoded-
view allocation. A fixed-width read remains one VM instruction and one
cancellation boundary. Exp-Golomb reads retain their existing 127-bit internal
bound and cancellation behavior. The existing mapped-view depth limit remains
reserved for nested transformation execution; constructing a reader over one
already resolved absolute mapping does not consume an additional view frame.

This slice does not yet accept the provisional `view`, `forward`, `skip`, or
`slice` format-language syntax. It also does not define which H.264 byte
sequences are excluded or how excluded spans appear in the analysis tree. Those
codec and presentation rules are the next separately documented M4 slice.

## Consequences

Syntax fields can be decoded directly from a logical view that crosses source
gaps while keeping one logical range and exact disjoint source spans. The raw
media remains the only byte store, and all existing selection and diagnostic
paths continue to use `FieldLocation` as their coordinate authority.

The executor rejects mismatched reader and mapping inputs before creating a
structure node or consuming source data. Reads that encounter a failure in a
later mapped span remain transactional, preserving the same partial-result
boundary as direct reads.

The backing representation may still require storage proportional to mapping
segments. Transformation builders must therefore coalesce adjacent forwarded
spans and enforce their own input, segment, and work limits. This ADR does not
permit an unbounded transformation to scan or index an entire large source
eagerly.

No accepted 0.1 syntax or direct-read behavior is deprecated. Direct mappings
remain the one-span case of the same execution contract.

## Considered Options

- Copy each logical view into a byte buffer: simple for readers, but duplicates
  large inputs, obscures source failures, and requires a second coordinate
  authority.
- Add an H.264-specific reader: smaller initially, but puts codec escape rules in
  the core read API and cannot serve later mapped container or codec views.
- Keep the reader direct and make the VM stitch fields itself: duplicates read,
  seek, error, and transaction semantics in the rules runtime instead of keeping
  them in the bounded core reader.
