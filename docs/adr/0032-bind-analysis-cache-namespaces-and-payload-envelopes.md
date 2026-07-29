# Bind Analysis Cache Namespaces And Payload Envelopes

Status: Accepted
Date: 2026-07-29

## Context

ADR-0029 deliberately made `PagedCache` accept an opaque caller namespace and
opaque page bytes while source and rule identity were unfinished. ADR-0030 now
provides complete package identity, and ADR-0031 provides a versioned source
fingerprint. Persistent cache reuse still needs one construction path that
cannot omit the entry point, SQLite schema, or owner payload formats.

SQLite namespace separation alone does not make page bytes self-validating. A
page copied under another namespace, decoded as the wrong page kind, written by
an incompatible owner version, truncated, or corrupted must fail before its
body reaches a scanner, tree, or materialized-result decoder.

## Decision

`RuleEntryPointIdentity` is the complete rule execution identity: package ID,
package version, package content SHA-256, and entry-point ID. Construction
validates every component. Catalog restore must still resolve this exact tuple
and diagnose missing, conflicting, or incompatible installed content.

`AnalysisCacheNamespace` version 1 is created only from:

- one validated `SourceFingerprint`, including its algorithm version and
  mode-specific fields;
- one complete `RuleEntryPointIdentity`;
- `PagedCache::schemaVersion()`;
- the cache namespace-format version;
- the payload-envelope format version; and
- the progressive-index and materialized-result payload-format versions.

The builder applies domain separation and length framing, then publishes
`sv-cache-v1-sha256:HEX`, where `HEX` is the SHA-256 of the complete
tuple. Filesystem paths and `RandomAccessSource::identity()` are not inputs.
Unsupported or zero versions are rejected. The text fits the existing SQLite
namespace bound and remains the value passed to `PagedCache::open`.

Each stored page body uses `AnalysisCachePayloadEnvelope` version 1. Its fixed
96-byte big-endian header contains:

- an eight-byte magic and envelope version;
- a closed page-kind code and that owner's payload version;
- reserved zero bytes;
- the 32-byte analysis-cache namespace digest;
- the payload length; and
- the payload SHA-256.

The remaining bytes are the owner-defined payload, limited so header plus body
never exceeds the existing 64 KiB page bound. Decode requires the expected
namespace and page kind and rejects unknown versions, namespace/kind mismatch,
nonzero reserved fields, invalid lengths, truncation, trailing bytes, and
payload digest mismatch. `PagedCache` remains unaware of this format and stores
the complete envelope as opaque bytes.

The envelope versions a payload boundary; it does not claim that analyzer state
already has a durable body representation. Scanner, mapper, tree, context, and
identifier serializers plus a background cache owner remain separate work.

## Consequences

Production cache users now have one deterministic namespace that binds every
available durable identity and storage-format version. Changing source content
or sampled metadata, package ID/version/hash, entry point, SQLite schema,
namespace framing, or either payload version selects another namespace.
Changing the envelope version also selects another namespace.

Page owners get bounded corruption and compatibility checks before decoding
their own data. A valid envelope proves only framing, namespace association,
kind, version, length, and bytes; semantic body validation remains the owner's
responsibility.

No path-only API is introduced, and an existing opaque namespace supplied
directly to `PagedCache` still does not authorize cross-session reuse. Persistent
analyzer recovery remains unavailable until the owner and its versioned body
serializers are connected.

## Considered Options

- Concatenate readable identity strings: ambiguous delimiters and omitted
  fields are easy to introduce as the tuple evolves.
- Put source and rule columns into SQLite: couples a generic page store to rules
  and requires a schema migration for every identity field.
- Trust only the SQLite namespace: does not detect copied, mislabeled, stale, or
  corrupt page bytes before owner decoding.
- Version only the envelope: different index and result bodies evolve
  independently and need separate versions.
- Serialize the live H.264 analyzer in the same change: its scanner, mapper,
  tree, queue, context, and identifier state still require explicit contracts.
