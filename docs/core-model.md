# Core Source And Bit Model

Status: Phase 1 implementation baseline. This English document is normative;
the maintained [Chinese companion](zh-CN/core-model.md) is provided for
accessibility.

## Bit Coordinates

A source bit address is an absolute zero-based bit offset in the unchanged
media source. Within each byte, bit offset 0 is the most-significant bit and
bit offset 7 is the least-significant bit. For example, source address
`byte=2, bit=3` is absolute bit offset 19.

Source spans and logical ranges are half-open: `[start, start + length)`. A
range is invalid if computing its exclusive end would overflow 64 bits. Empty
ranges are valid boundaries, but source mappings do not contain empty spans.

A logical address is meaningful only together with its logical view identity.
Ranges from one view cannot be resolved through another view's mapping.

## Source Mappings

A source mapping represents one complete logical view as an ordered list of
absolute source spans. It has these invariants:

- every mapped source span is non-empty;
- spans are ordered, do not overlap, and preserve source order;
- adjacent spans are coalesced;
- the logical view length equals the checked sum of source-span lengths.

Resolving a logical range returns a field location containing the original
logical range and the exact source spans that back it. A logical range crossing
an excluded source byte therefore resolves to multiple spans. For example,
logical bits `[8, 24)` in a view backed by source bits `[0, 16)` and `[24, 40)`
resolve to source bits `[8, 16)` and `[24, 32)`.

Mappings reject overflow, overlapping or out-of-order spans, a range belonging
to another view, and a range outside the logical view. Values without an exact
mapping must later be represented as computed fields rather than source-backed
locations.

## Read-Only Sources

The random-access source interface exposes only size, identity, and `readAt`.
The local-file implementation opens the media source with Qt's read-only mode
and provides no write or resize operation. Concurrent random reads are
serialized around file seeking without loading the complete file into memory.

A read reports one of:

- `complete`: the requested destination was filled;
- `end-of-source`: the read reached or started beyond the current source end;
- `error`: the source could not perform the read.

An end-of-source read may contain a valid partial byte count. Callers must not
interpret bytes beyond that count.

## Paged Source Access

`SourcePager` exposes a bounded page view over a `RandomAccessSource`. Each page
is at most 64 KiB and is addressed by a checked page index; loading one page
does not read adjacent pages or retain a cache of earlier pages. A source whose
size is not a multiple of 64 KiB has a final page containing only the declared
remaining bytes. A page that reaches the declared source end is reported as
`end-of-source` while remaining a successful page result.

If a source reports `end-of-source` before the bytes declared by
`sizeBytes()` have been returned, the page is an error rather than a valid
short page. Source errors preserve only the bytes explicitly reported by the
source and never expose the unwritten part of the page buffer. An out-of-range
page is an empty end-of-source result; an overflowing page coordinate is an
error.

## Bit Reader

The bounded bit reader consumes 1 through 64 bits at a time in
most-significant-bit-first order. Its position is relative to its declared
source span. A read first checks the declared boundary, then requests only the
source bytes needed for that value.

Successful reads advance by the requested width. Invalid widths, reads beyond
the declared range, unexpected source truncation, and source errors do not
advance the reader. This transactional behavior lets a parser retain all
previous complete fields and attach a precise diagnostic at the unresolved
field boundary.

Valid example: reading 3 bits from `0b10110010` yields `0b101`, then reading 5
bits yields `0b10010`. Invalid examples include requesting 0 or 65 bits and
requesting a 12-bit value from an 8-bit bounded range.
