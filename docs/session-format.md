# StreamView Session Format

Status: accepted version 1 contract. This English document is normative; the
maintained [Chinese companion](zh-CN/session-format.md) is provided for
accessibility.

## Purpose And Boundary

A `.svsession` file is a compact record of user work against one read-only
local media file and one exact rule entry point. It never contains media bytes,
SQLite cache pages, a live analyzer checkpoint, or installed rule-package
content. Rebuildable progressive indexes and materialized results remain
outside this document.

The document path is only the locator used to reopen the media file. The saved
source identity is descriptive. Neither value authorizes reuse. StreamView
applies bookmarks, annotations, expanded paths, or view state only after a
fresh fingerprint from the newly opened file handle equals the complete saved
fingerprint.

## JSON Version 1

Version 1 is UTF-8 JSON with a maximum encoded size of 1 MiB and a maximum
nesting depth of 256. Every object has a closed field set. Missing fields,
unknown fields, duplicate keys, wrong JSON types, malformed values, and an
unsupported schema or fingerprint version are rejected. Duplicate comparison
uses decoded JSON strings, so `"schemaVersion"` and `"\u0073chemaVersion"`
also conflict.

The canonical writer uses this shape:

```json
{
    "schemaVersion": 1,
    "source": {
        "path": "/media/fixture.264",
        "identity": "/media/fixture.264",
        "fingerprint": {
            "version": 1,
            "mode": "full-content-sha256",
            "sizeBytes": "5",
            "sha256": "1111111111111111111111111111111111111111111111111111111111111111"
        }
    },
    "rule": {
        "packageId": "org.example.packet",
        "packageVersion": "1.2.3",
        "contentSha256": "2222222222222222222222222222222222222222222222222222222222222222",
        "entryPointId": "packet"
    },
    "bookmarks": [
        { "label": "Header", "sourceBitOffset": "8" }
    ],
    "annotations": [
        { "text": "Check this flag", "sourceBitOffset": "9", "bitLength": "3" }
    ],
    "expandedPaths": ["root/packet"],
    "view": {
        "rawPageIndex": "0",
        "rawDisplayMode": "combined",
        "selectedSourceBitOffset": "10",
        "selectedAnalysisPath": "root/packet/value"
    }
}
```

All unsigned 64-bit quantities are canonical decimal strings: `0` or a
nonzero digit followed by decimal digits, with no sign or leading zero. The
sampled fingerprint modification time is a canonical signed decimal string;
`+`, `-0`, and leading zeroes are rejected. Using strings avoids precision loss
through the JSON binary64 number model. SHA-256 values are exactly 64 lowercase
hexadecimal characters without a `sha256:` prefix.

## Source Fingerprint

The `source.fingerprint` object is the complete version 1
`SourceFingerprint`:

- `version` is the JSON number `1`;
- `mode` is `full-content-sha256` or `sampled-sha256`;
- `sizeBytes` is the declared source size;
- `sha256` is the fingerprint digest; and
- `modificationTimeNanoseconds` is required only for sampled mode and forbidden
  for full-content mode.

The mode, size, modification-time presence, and digest must satisfy the
versioned fingerprint contract in
[ADR-0031](adr/0031-versioned-file-source-fingerprints.md). A fingerprint is
computed from the same read-only `FileSource` handle used by analysis. A
virtual source cannot be persisted by presenting a path-like `identity()`.

## Exact Rule Pin

The `rule` object is one complete `RuleEntryPointIdentity`: package ID,
canonical package version, package logical-tree content hash, and entry-point
ID. Restore performs exact catalog lookup with the running DSL and engine
versions. Missing content, another hash occupying the same ID and version,
an unknown entry point, incompatible DSL, and incompatible engine each remain
distinct failures. Restore never selects a newer or otherwise nearby version.

The bundled H.264 analyzer follows the same path: its package is loaded and
resolved through the catalog contract, and the resulting analyzer retains the
complete identity. A catalog-resolved installed package can therefore recreate
the analyzer under the same exact-pin rules.

## User State

Version 1 stores:

- up to 4,096 bookmarks, each with a non-empty label of at most 64 KiB UTF-8
  and one source bit that exists in the pinned source;
- up to 4,096 annotations, each with non-empty text of at most 64 KiB UTF-8 and
  one non-empty, non-overflowing source-bit range contained by the source;
- up to 16,384 unique, non-empty expanded analysis paths of at most 16 KiB
  UTF-8 each; and
- one view state containing an in-range 64 KiB raw page index, one of `hex`,
  `binary`, or `combined`, and nullable source-bit and analysis-path selections.

The source path and identity are each non-empty and at most 16 KiB UTF-8. A
selected source bit must exist. A selected analysis path, when present, is
non-empty and at most 16 KiB UTF-8. JSON `null`, not omission, represents each
absent selection.

## Save And Restore

Save uses `QSaveFile` with direct-write fallback disabled. StreamView writes a
sibling temporary file and replaces the destination only when `commit()`
succeeds; inability to provide that atomic replacement is an error.

Restore is ordered and transactional with respect to the active application
session:

1. parse and validate the complete session document;
2. open `source.path` read-only;
3. compute and compare the complete source fingerprint;
4. resolve the complete rule identity and compatibility in the catalog;
5. compile and construct the analyzer from that resolved package entry; and
6. attach the validated user state to the new session.

Failure at any earlier step constructs no replacement session and applies no
saved user coordinate. Cache lookup, cache payload restoration, Save As UI,
dirty-state prompts, and source relocation are separate contracts.
After a successful restore, a caller may enable the write-only analysis cache
with the already verified fingerprint; this does not add cache data to the
session document and does not perform cache lookup or restoration.
