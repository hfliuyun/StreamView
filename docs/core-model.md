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

## Position-Aware Context Directories

`ContextDirectory` records completed source-backed definitions that later
syntax may reference. Its closed definition kinds cover H.264 SPS and PPS, AAC
AudioSpecificConfig, and ISO BMFF sample descriptions. A key combines that kind
with a host-assigned numeric scope and kind-specific value. Scope zero is the
natural standalone-stream scope; a container uses distinct stable scopes for
tracks whose context IDs may otherwise collide.

Each definition has a non-empty absolute source span, an analysis-node ID, a
monotonic definition ID, and exact dependency-generation IDs. For a key and
query source position, the directory selects the definition whose exclusive
source-span end is the greatest end not after the query. A definition is not
visible while the query is inside that definition, becomes visible exactly at
its exclusive end, and never affects earlier positions.

Definitions for one key cannot overlap, but registration need not follow source
order. This permits lazy and known-offset analysis while preserving deterministic
position lookup. IDs follow append order, are never reused, and lookup returns
snapshots that remain valid after later registration.

A dependency must be the exact generation selected before the dependent
definition starts. Lookup rechecks that every bound dependency is still the
current generation at the consumer position. Redefinition therefore reports
`dependency-unavailable` for stale dependent context instead of falling back or
guessing. Resolution visits at most 64 definitions. Dependency cycles produced
by later cross-redefinitions and chains beyond that limit are reported
unavailable.

Malformed definitions are not registered and do not shadow the preceding valid
generation. Rejected registration changes no visible directory state. The
directory stores no format-specific payload, performs no source read, and uses
the analysis worker's existing single-writer ownership. The first consuming
format rule will connect one directory to its analysis session. Persistence and
SQLite paging are separate later contracts; see
[ADR-0028](adr/0028-resolve-context-generations-by-source-position.md).

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

## Durable File Fingerprints

`FileSource::fingerprint()` computes version 1 identity from the same open,
read-only file handle; it never treats the path-like source identity as durable.
Files up to 3 MiB use full-content SHA-256 with size. Larger files use size,
Unix-epoch nanosecond modification time, and SHA-256 over the concatenated
first, middle, and last 1 MiB windows. The middle window starts at
`floor((size - 1 MiB) / 2)`. Full hashing therefore reads no more bytes than
sampling, and either mode retains only a 64 KiB working buffer.

The implementation compares size and nanosecond modification time from the
same OS handle before and after hashing. A file whose size differs from the
open source, or whose snapshot changes during computation, is diagnosed as
changed rather than given a fingerprint. Small-file modification time is used
for this consistency check but is not stored in the durable value, so touching
unchanged content does not cause a mismatch. Unsupported metadata and I/O
errors are explicit. Virtual sources receive no path-identity fallback. See
[ADR-0031](adr/0031-versioned-file-source-fingerprints.md).

## Persistent Session Documents

`SessionDocument` is the compact, versioned user-state record for one local
file and one complete rule entry-point identity. Version 1 is a closed UTF-8
JSON schema containing the source path and descriptive identity, complete
`SourceFingerprint`, package ID/version/content hash and entry-point ID,
bookmarks, annotations, expanded analysis paths, and raw/selection view state.
All 64-bit quantities use canonical decimal strings so JSON binary64 does not
lose source-coordinate precision. The parser bounds document size, nesting,
text and collection counts, and rejects duplicate, missing, unknown, mistyped,
unsupported, malformed, or out-of-source values.

Saving uses `QSaveFile` with direct-write fallback disabled. Restore validates
the whole document, opens the local file read-only, compares a fresh
same-handle fingerprint, performs exact compatible catalog lookup, constructs
the analyzer from that resolved entry, and only then attaches user state. A
failure returns no replacement session and applies no saved coordinate. The
bundled H.264 analyzer also retains the complete catalog-resolved identity.
Path-like identity never authorizes restore, and virtual sources cannot be
persisted through that fallback. Cache pages and live analyzer state are not
part of `.svsession`; see [the session format](session-format.md) and
[ADR-0033](adr/0033-save-exact-analysis-sessions-as-atomic-json.md).

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

## Paged Analysis Cache

`PagedCache` stores rebuildable progressive-index and materialized-result data
as opaque pages in SQLite WAL. It does not cache media-source bytes or interpret
the payload encoding. A caller opens one thread-affine cache instance with a
database path and an opaque namespace, then reads exact page keys or commits a
complete batch. An arbitrary namespace remains only a partition and never
authorizes cross-session reuse. Production reuse uses
`AnalysisCacheNamespace`, a domain-separated SHA-256 over the validated source
fingerprint, complete package ID/version/content hash and entry-point ID,
SQLite schema, namespace-format version, and both payload-format versions.
The payload-envelope format version is also bound. Path-like source identity is
not an input.

Owner data is wrapped in a version 1 `AnalysisCachePayloadEnvelope` before
storage. Its fixed header binds the namespace digest, closed page kind, that
kind's payload version and payload length, then checks body integrity with
SHA-256. The envelope plus body remains within the 64 KiB page limit. Decode
rejects wrong namespace or kind, unknown versions, malformed framing,
truncation, trailing bytes, and digest mismatch before an owner interprets the
body. `PagedCache` itself still treats the complete envelope as opaque bytes.
See [ADR-0032](adr/0032-bind-analysis-cache-namespaces-and-payload-envelopes.md).

Each payload contains 1 through 64 KiB. One atomic commit contains 1 through
256 unique page keys and therefore at most 16 MiB. A successful commit exposes
every replacement together; a failed commit exposes none. Missing pages are
normal cache misses. Wrong-thread access and invalid keys are rejected before
storage is touched. One live instance owns its database path exclusively; a
second open fails until the owner closes or a stale process lock is recovered.

Cache open verifies the QSQLITE runtime driver, WAL configuration, StreamView
application ID, schema version, required schema, and SQLite integrity. SQLite
rolls back an interrupted transaction, while a persistent marker lets the next
open identify and remove batches abandoned by a process crash. Marker-cleanup
failure aborts open. With `synchronous=NORMAL`, this is not a power-loss
durability promise. The cache stores committed rebuildable pages only; it does
not restore a live analyzer, scanner, mapper, analysis tree, or context
directory. Those owners still need versioned body serializers and background
thread ownership before persistent analyzer recovery is available.

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
