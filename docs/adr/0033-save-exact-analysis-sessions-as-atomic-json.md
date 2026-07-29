# Save Exact Analysis Sessions As Atomic JSON

Status: Accepted
Date: 2026-07-29

## Context

ADR-0007 separates persistent user work from immutable media. ADR-0030 defines
complete package and entry-point identity, ADR-0031 defines the durable local
file fingerprint, and ADR-0032 binds those values into cache namespaces. M5
still needs the compact `.svsession` representation and the point at which
saved source coordinates and rule selection become safe to reuse.

Path identity is inadequate. A file can change in place, and a package version
can be republished with other content. JSON numbers are also inadequate for
arbitrary 64-bit bit coordinates because common JSON implementations store
them as binary64. A permissive document with unknown or duplicate fields would
make migrations and hostile-file diagnostics ambiguous. Saving directly over
the only copy would risk destroying user work on a short write or crash.

The current H.264 analyzer validated the bundled package but then compiled a
detached source string and discarded package identity. Restoring an exact pin
therefore also requires an analyzer construction path that consumes an exact,
compatible catalog result.

## Decision

The normative version 1 format is maintained in
[the session format](../session-format.md). It establishes:

- a closed, bounded UTF-8 JSON schema selected by `schemaVersion: 1`;
- canonical decimal strings for all 64-bit source coordinates and sizes;
- the complete version 1 `SourceFingerprint`, including sampled-mode
  nanosecond modification time;
- the complete `RuleEntryPointIdentity`, including package logical-tree hash;
- bounded bookmarks, annotations, expanded analysis paths, and raw/selection
  view state;
- rejection of missing, unknown, duplicate, mistyped, out-of-range, or
  unsupported values before user state is exposed; and
- atomic replacement through `QSaveFile` with direct-write fallback disabled.

Restore opens the saved source path, computes the fingerprint from that same
read-only `FileSource` handle, and requires full equality before exact catalog
lookup. It then resolves package ID, version, content hash, entry point, DSL
compatibility, and engine compatibility without fallback. Only the resolved
package entry can construct the analyzer. The bundled H.264 package uses this
same route and its analyzer retains the complete resolved identity.

Validated user state is attached only after source comparison, catalog lookup,
and analyzer construction all succeed. A failure returns no replacement
session. The saved source identity remains descriptive; neither it nor the path
can replace the fingerprint. Virtual sources cannot be persisted by returning
a path-like identity.

The document contains no media bytes, rule package bytes, cache pages, or live
analyzer state. Those data are immutable external input or rebuildable owner
data and have separate versioning and lifecycle contracts.

## Consequences

- An old session either finds the exact package content it names or fails with
  a specific catalog status. It never falls forward to another rule version.
- Media changes at the same path are detected before saved coordinates are
  applied. Small-file metadata-only changes remain valid under ADR-0031.
- Large integer values round-trip on every supported Qt platform without JSON
  precision loss.
- Session writes require an atomic sibling-temporary-file replacement. A
  filesystem that cannot provide it causes save to fail rather than silently
  weakening durability.
- Parser and collection bounds limit hostile document work, while closed
  objects make future schema changes explicit.
- Save/Save As actions, dirty-state prompts, source relocation, rule-management
  UI, the background cache owner, and live analyzer restoration remain later
  work. Cache owner body formats are specified separately by ADR-0034.

## Alternatives Rejected

- Store only source and package paths: neither proves byte identity.
- Store package ID and version without content hash: republished content could
  silently reinterpret old user work.
- Store 64-bit coordinates as JSON numbers: binary64 cannot represent every
  value exactly.
- Permit unknown fields for forward compatibility: versioned closed schemas
  provide clearer migrations and reject misspelled security-relevant fields.
- Save cache pages in `.svsession`: they are rebuildable, separately versioned,
  and can dwarf compact user state.
- Write the destination with `QFile`: partial replacement can destroy the last
  valid session.
