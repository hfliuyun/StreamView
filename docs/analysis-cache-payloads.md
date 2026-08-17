# Analysis Cache Owner Payloads

Status: Normative
Version: 1

This document specifies the owner-defined bodies stored inside version 1
`AnalysisCachePayloadEnvelope` values. It defines one body for H.264
progressive-index records and one body for stable materialized-result nodes.
Both are rebuildable cache representations. Neither is a live analyzer
checkpoint.

## Common Encoding

All integers are unsigned big-endian values unless a field is explicitly
described as signed. Sizes and offsets are in bytes for progressive-index
records and in bits for field locations. Reserved values must be zero. Flag
bits and enumeration codes are closed; a decoder must reject unknown values.

Every body must fit within
`AnalysisCachePayloadEnvelope::maximumPayloadBytes()` (65,440 bytes), so the
96-byte envelope plus body fits one 64 KiB `PagedCache` page. The body repeats
the page key's `streamId` and `pageIndex`. A decoder must take the complete
expected `PagedCachePageKey`, require the correct page kind, and compare both
repeated values. Moving a valid body to another key is a cache miss or corrupt
entry, never an authorization to reuse it. Both key coordinates must be in the
nonnegative signed 64-bit range accepted by `PagedCache` and SQLite.

Strings use a 32-bit byte length followed by strict UTF-8. Each string is at
most 32 KiB and must round-trip without replacement characters. Encoders reject
`QString` values that do not represent Unicode scalar text.

The required write stack is:

1. encode and semantically validate the owner body;
2. wrap it with `AnalysisCachePayloadEnvelope` for the expected namespace and
   page kind; and
3. commit the complete envelope under the same full page key.

The read stack is the reverse: read the exact page key, validate the envelope,
then decode the body with that same full key. Envelope validation alone does
not bind `streamId` or `pageIndex` and is therefore not a complete cache read.

## Progressive-Index Body

The version 1 progressive-index header is 56 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | magic `SVPIDX\0\0` |
| 8 | 4 | format version `1` |
| 12 | 4 | flags; bit 0 is `endOfSource` |
| 16 | 8 | stream ID |
| 24 | 8 | page index |
| 32 | 8 | first global record index |
| 40 | 8 | indexed-through byte offset |
| 48 | 4 | record count |
| 52 | 4 | reserved zero |

Each record is 48 bytes:

| Relative offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | start-code byte offset |
| 8 | 4 | start-code length, exactly 3 or 4 |
| 12 | 4 | reserved zero |
| 16 | 8 | NAL-unit byte offset |
| 24 | 8 | NAL-unit byte length |
| 32 | 8 | trailing-zero byte offset |
| 40 | 8 | trailing-zero byte length |

Record count may be zero and is bounded by the remaining page capacity. The
first record index plus record count must not overflow 64 bits. For every
record, checked arithmetic must prove:

- `nalUnitOffset == startCodeOffset + startCodeLength`;
- `trailingZeroOffset == nalUnitOffset + nalUnitLength`;
- the complete record ends at or before `indexedThroughByteOffset`; and
- records are ordered and do not overlap.

Decode reconstructs source spans from byte offsets and lengths. A zero-length
NAL unit or trailing-zero run has no span; a nonzero range must convert to bits
without overflow and must exactly match its reconstructed span. The encoding
does not contain a scanner's pending prefix, buffered source bytes, pending
start code, cancellation token, mapper state, or queued analyzer work.

## Materialized-Result Body

The version 1 materialized-result header is 40 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | magic `SVMATR\0\0` |
| 8 | 4 | format version `1` |
| 12 | 4 | reserved zero |
| 16 | 8 | stream ID |
| 24 | 8 | page index |
| 32 | 4 | node count |
| 36 | 4 | reserved zero |

A body contains 1 through 1,024 nodes. Each node begins with this fixed header,
then the variable fields in the listed order:

| Size | Field |
| ---: | --- |
| 8 | stable node ID |
| 8 | parent ID, or zero for no parent |
| 4 | node-kind code |
| 4 | materialization-state code |
| 4 | value-kind code |
| 4 | flags: bit 0 location, bit 1 specification, bit 2 target format, bit 3 window metadata, bit 4 container child struct index |
| variable | name, type name, and description strings |
| variable | standard and clause strings when flag bit 1 is set |
| variable | target-format string when flag bit 2 is set |
| 24 | window entry struct index (`u32`), count-field index (`u32`), entry size in bits (`u64`), and entry count (`u64`) when flag bit 3 is set |
| 4 | container child struct index when flag bit 4 is set |
| variable | encoded value |
| variable | field location when flag bit 0 is set |
| 4 | diagnostic count |
| variable | diagnostics |

Node-kind codes are `1` root, `2` structure, `3` syntax field, `4` computed
field, `5` compressed payload, and `6` region. State codes preserve the core
enumeration plus one: `1` lazy, `4` cancelled, `5` unsupported, `6` invalid,
and `7` materialized are stable. Known transient codes `2` indexing and `3`
waiting-dependency are invalid in a cache body. Other codes are unsupported.

Value-kind codes are `0` absent, `1` Boolean, `2` unsigned 64-bit, `3` signed
64-bit, and `4` string. Boolean, unsigned, and signed values occupy eight bytes;
Boolean is exactly zero or one. Signed values store the two's-complement bit
pattern. No implicit `QVariant` conversion is allowed.

### Locations

A location has this layout:

| Size | Field |
| ---: | --- |
| 8 | logical view ID |
| 8 | logical start bit offset |
| 8 | logical bit length |
| 4 | source-span count |
| 4 | reserved zero |
| 16 each | source start bit offset and bit length |

At most 1,024 source spans are allowed. Logical range arithmetic must not
overflow. Every source span must be nonempty, ordered, nonoverlapping, and
canonically separated from the preceding span; adjacent spans must be merged
before encoding. Their total length must equal the logical length. A zero-length
logical range has no spans. Syntax fields require a location, while computed
fields must not have one.

Target format, window metadata, and container child struct index are optional
lazy-region metadata. A version 1 body written before these flags existed has
all three bits clear and decodes each value as absent. The field order above is
normative when multiple extension flags are set.

### Diagnostics

Each node has at most 256 diagnostics. A diagnostic contains four 32-bit values
(diagnostic code, severity code, flags, reserved zero), then message and field
path strings, followed by a location when flag bit 0 is set. Diagnostic codes
are the core enumeration plus one in the closed range 1 through 7; severities
are the core enumeration plus one in the closed range 1 through 3. The message
must be nonempty.

### Node Topology

Node IDs are nonzero and contiguous within a page. A root is exactly node 1 and
has no parent. Every non-root has a nonzero parent ID lower than its own ID. A
later page may refer to a parent from an earlier page, so complete cross-page
reachability is an owner-level validation after all required pages are loaded.
Names are nonempty. A specification, when present, has nonempty standard and
clause strings.

The body preserves stable result records only. It does not serialize child
vectors, a mutable `AnalysisTree`, the next-node allocator, scanner or mapper
state, deferred result queues, cancellation state, context-directory payloads,
or cache-thread ownership. A consumer may rebuild a presentation snapshot from
a separately complete, validated key set, but cannot resume a live analyzer
from it.

## Background Owner Stack

`AnalysisCacheOwner` accepts typed pages for one validated namespace. Write
submission performs body encoding, envelope encoding, full-key checks, duplicate
checks, and the 256-page batch bound before retaining a request. The dedicated
worker commits only those bounded encoded pages. Read submission accepts one
exact full key; the worker reads it, validates the envelope for that namespace
and kind, then decodes the body against the same key.

The owner returns missing for absent storage and corrupt for any envelope or
body failure. Neither outcome returns a partial typed page. Queue pressure,
shutdown, and storage errors are separate explicit outcomes, and callers rebuild
or disable the optional cache without changing a valid live analysis.

This exact-page stack does not define page discovery. In particular, a missing
version 1 materialized page is not a final-page marker. A consumer must already
have a separately validated complete key set before performing cross-page
reachability validation or publishing a presentation snapshot.

Production H.264 sessions may publish version 1 pages through this stack. Each
actual scanner batch exposes at most one stable progressive-index update; the
exclusive frontier is the greatest completed record end, or source size only
when scanning is complete. Stream and initial page indexes are zero. A terminal
stable tree is exported in node-ID order into the largest consecutive prefixes
accepted by this codec, with no empty pages, then submitted as one batch of at
most 256 pages. This producer behavior is write-only: it supplies neither a
complete-key manifest nor cached snapshot or live analyzer recovery. See
[ADR-0036](adr/0036-publish-stable-session-cache-pages-without-restoring-execution.md).

## Failure Handling

Encoding distinguishes invalid semantic input, unsupported closed values, and
page/text overflow. Decoding distinguishes malformed bodies, a repeated page
key mismatch, unsupported body versions, and unsupported closed values. Any
failure discards the entire page result. Callers treat it as unusable cache data
and rebuild from the immutable source and exact rule identity; they must not
partially attach decoded nodes or silently reinterpret the bytes.

See [ADR-0034](adr/0034-cache-stable-analysis-results-not-live-state.md) and
[ADR-0035](adr/0035-own-analysis-cache-on-a-bounded-background-queue.md).
