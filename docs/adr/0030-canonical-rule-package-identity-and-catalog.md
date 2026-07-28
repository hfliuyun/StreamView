# Canonical Rule Package Identity And Catalog

Status: Accepted
Date: 2026-07-28

## Context

ADR-0014 separates application, language, and package versions; ADR-0015 keeps
distribution local and requires exact session pinning; ADR-0016 selects TOML
and deterministic ZIP provisionally. M5 now needs executable contracts for the
manifest, content hash, compatibility range, catalog identity, local directory
import, `.svrule` extension, and hostile package boundaries.

Those choices must precede persistent cache and session integration. A package
path or version is insufficient identity: directory and archive forms must
agree, two different payloads must not occupy one version silently, and an old
session must not fall forward to a newer installed rule. Archive-byte hashing
would make harmless ZIP representation differences change identity and would
give directory imports no natural equivalent.

Qt Core supplies SHA-256 but no public general ZIP API or TOML parser. Pulling a
compression runtime into the first package slice would add deployment and
supply-chain work before compressed packages provide product value. The input
format nevertheless needs strict, cross-platform path and size rules from its
first implementation.

## Decision

The normative version 1 contract is maintained in
[the rule-package format](../rule-packages.md). It fixes `.svrule` as the
package extension and defines:

- a closed TOML 1.0 `rule.toml` schema with package metadata, an exact
  `MAJOR.MINOR` language contract, one `>=LOWER <UPPER` engine range,
  entry-point coverage, detector metadata, localized documentation, and an
  explicitly empty first-release dependency list;
- strict canonical semantic versions without build metadata;
- a logical package tree made only of bounded regular files under canonical
  portable ASCII paths;
- a domain-separated, length-framed SHA-256 over sorted logical paths and exact
  file bytes;
- package identity `(package ID, package version, content hash)` and entry-point
  identity that additionally contains the entry-point ID;
- exact catalog lookup, coexistence across versions, idempotence for the same
  complete identity, and transactional conflict when the same ID and version
  arrive with different content;
- a deterministic store-only ZIP32 representation with fixed headers,
  timestamps, permissions, order, and no optional fields; and
- bounded validation plus staged, read-only, content-addressed installation.

The store-only archive choice is intentional. It is valid ZIP, deterministic
without a compression library, and cannot expand beyond its already bounded
input bytes. The importer accepts only the canonical representation. A future
compressed archive version requires a new manifest/package-format version and
an ADR covering dependencies, decompression streaming, ratios, and canonical
encoder behavior.

Version 1 package paths are ASCII rather than normalized Unicode. Non-ASCII
paths are rejected, along with case-fold aliases, traversal, links, and special
files. Package contents may still be arbitrary UTF-8 or bounded binary data.
This keeps identity identical on Windows, macOS, and Linux while leaving a
future Unicode path profile possible through an explicit format revision.

Installed or bundled packages may be cataloged even when they are incompatible
with the running engine so upgrades can retain exact historical content.
Activation, analysis, and session restore reject an incompatible exact identity
with a diagnostic. They never select another version heuristically.

Source fingerprints, cache namespace framing, versioned cache payloads, and
`.svsession` serialization will consume the complete package identity in a
later M5 slice. This decision does not authorize cache reuse by path or package
version alone.

## Consequences

Directory and `.svrule` imports converge on one validated tree and one content
hash. Catalog conflicts are visible, versions coexist without overwrite, and
sessions have an exact identity to pin. The bounded store-only ZIP reader and
writer can be implemented with Qt Core and small local binary framing code.

Whitespace-only manifest changes alter the hash because exact manifest bytes
belong to the package tree. This is deliberate: the digest identifies supplied
content, not only parsed behavior. Repacking the unchanged tree does not alter
the hash.

The strict first version omits dependency graphs, compressed archives, Unicode
paths, signatures, trust elevation, remote indexes, and automatic updates.
Adding any of them changes security or identity policy and requires a versioned
contract rather than permissive parsing.

## Considered Options

- Hash raw ZIP bytes: gives directory imports no stable equivalent and makes
  metadata or compressor differences change package identity.
- Hash only parsed manifest and entry-point sources: permits untracked package
  documentation or test data to change under one identity.
- Use package ID and version as the catalog key: silently aliases republished
  content and cannot pin reproducible sessions.
- Let different hashes coexist under the same ID and version: makes a textual
  version ambiguous; version conflict is safer and asks authors to publish a
  new version.
- Accept arbitrary safe ZIP files and normalize them after import: broadens the
  parser and leaves the distributed artifact itself non-reproducible.
- Use Qt private ZIP classes: binds the format to private APIs across the mixed
  Qt 6.10/6.11 CI matrix.
- Add compressed ZIP immediately: requires another dependency and more zip-bomb
  policy without helping the small text-first rule packages.
- Normalize Unicode paths: normalization, case behavior, and filesystem aliases
  differ across supported platforms; the ASCII version 1 profile is explicit
  and portable.
