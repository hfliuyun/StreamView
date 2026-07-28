# StreamView Rule Package Format

Status: accepted version 1 contract. This English document is normative; the
maintained [Chinese companion](zh-CN/rule-packages.md) is provided for
accessibility.

## Package Model

A rule package is one validated logical tree of regular files. The same tree
may be supplied as a local directory or as one deterministic `.svrule` ZIP
container. Both forms produce the same content hash and package identity.

Version 1 packages are self-contained data. They contain DSL sources,
documentation, and distributable test data, but no native executable code,
symbolic links, runtime dependency resolution, or network references. Package
content never receives file, process, environment, network, pointer, or native
library access.

Every package root contains exactly one `rule.toml` manifest. Other accepted
files live below `src/`, `docs/`, or `tests/`. A manifest source path names a
`.svfmt` regular file below `src/`; a documentation path names a regular file
below `docs/`. Unreferenced data is still validated and included in the content
hash.

## Manifest Version 1

`rule.toml` is UTF-8 TOML 1.0 without a byte-order mark. Manifest version 1 has
the following schema. All shown scalar and table keys are required except
`detector`; the complete `documentation` array is optional. Unknown keys or
tables are rejected.

```toml
manifest-version = 1

[package]
id = "org.streamview.h264"
version = "0.1.0"
authors = ["StreamView contributors"]
license = "MIT"
dependencies = []

[compatibility]
language = "0.1"
engine = ">=0.1.0 <0.2.0"

[[entrypoints]]
id = "annex-b"
format = "video.h264.annex-b"
source = "src/h264_annex_b.svfmt"
profiles = ["baseline", "main", "high"]
depth = "nal-header"
detector = "h264-annex-b"

[[documentation]]
language = "en"
path = "docs/en/h264-annex-b.md"

[[documentation]]
language = "zh-CN"
path = "docs/zh-CN/h264-annex-b.md"
```

The fields mean:

- `manifest-version` selects this schema and must be the integer `1`.
- `package.id` is the stable package name. It contains 2 through 8 lowercase
  ASCII dot-separated segments. Each segment begins with `a-z`, continues with
  `a-z`, `0-9`, or `-`, and contains at most 32 characters. The complete ID
  contains at most 128 characters.
- `package.version` follows SemVer 2.0.0 precedence and the grammar
  `MAJOR.MINOR.PATCH[-PRERELEASE]`. Core components are decimal integers with no
  leading zero unless they are zero. A prerelease is one or more dot-separated
  identifiers containing ASCII letters, digits, or `-`; an all-numeric
  identifier has the same no-leading-zero rule. Empty identifiers and `+` build
  metadata are rejected. The canonical text is retained as the catalog version.
- `package.authors` contains 1 through 16 non-empty display strings, each at
  most 256 UTF-8 bytes. Author claims do not grant trust or permissions.
- `package.license` is non-empty SPDX license-expression text of at most 256
  ASCII characters. Version 1 validates only that representation and does not
  claim registry lookup, expression canonicalization, or license approval.
- `package.dependencies` must be an empty array. Non-empty dependency graphs
  and runtime dependency resolution are not part of version 1.
- `compatibility.language` declares one canonical `MAJOR.MINOR` DSL contract.
  During language major zero it must equal the running engine's
  `languageVersion()` before activation. At language major one and later, the
  running contract is compatible when its major is equal and its minor is not
  older than the declared minor.
- `compatibility.engine` has the exact form `>=LOWER <UPPER`, with one ASCII
  space between two canonical stable semantic versions. `LOWER` must precede
  `UPPER`; the running `streamview::core::version()` is compatible exactly when
  it lies in that half-open interval under SemVer precedence. This is the engine
  API contract version, not an artifact's alpha, beta, or release-candidate
  tag; those tags do not change compatibility unless the engine contract itself
  changes. A prerelease string is not a valid running engine-contract version.
- `entrypoints` contains 1 through 64 entries. Each `id` is unique within the
  package and uses 1 through 64 lowercase ASCII letters, digits, and hyphens,
  beginning with a letter. `format` uses the same dotted identifier grammar as
  a package ID. `source` is a canonical package path below `src/` ending in
  `.svfmt`. `profiles` contains 1 through 64 unique tokens using the entry-ID
  grammar. `depth` is one such token describing declared analysis
  coverage. Optional `detector` names a host detector by the entry-ID grammar;
  it is metadata, not executable package code. Version 1 knows
  `h264-annex-b`; an unknown detector leaves the package catalogable but makes
  automatic detection for that entry explicitly unsupported. With no detector,
  the entry remains available for manual selection. Entry-point IDs and source
  paths are both unique.
- `documentation` contains zero through 64 records. `language` is a unique,
  ASCII tag matching `[a-z]{2,3}(-[A-Z]{2})?` and `path` is a unique canonical
  package path below `docs/`.

TOML table order and insignificant syntax do not change the parsed fields, but
the manifest's original bytes are part of the logical tree. Editing whitespace
therefore intentionally changes the package content hash.

## Paths And Tree Limits

Every logical path is a relative, slash-separated ASCII string. Each component
contains only `A-Z`, `a-z`, `0-9`, `.`, `_`, or `-`; is neither empty, `.` nor
`..`; and does not end in a dot or space. Backslashes, drive or UNC prefixes,
leading slashes, control characters, and non-ASCII path bytes are rejected
rather than normalized. `%` is outside the alphabet and names are never
percent-decoded. This conservative version 1
alphabet rejects Unicode normalization and platform case-folding aliases at
the boundary. Two paths must also be unique after ASCII case folding.

The installer accepts only regular files and real directories. It never follows
a symbolic link, junction, reparse-point alias, or other special filesystem
entry. Directory import opens each component relative to an already opened root
or parent handle with no-follow semantics, verifies the opened object is still
the expected bounded regular file or directory, and copies bytes from that
stable handle into private staging. It never validates one path then reopens
that path for hashing or installation. Hard-link relationships are not
preserved: each logical path contributes and installs its own copied bytes.

Archive entries represent files only; directory entries and Unix file types
other than a read-only regular file are rejected. Package data is never
executed. A source path must end in `.svfmt`. A filename is rejected after
ASCII case folding when it ends in `.exe`, `.com`, `.dll`, `.dylib`, `.bundle`,
`.app`, `.msi`, `.sys`, or `.drv`, or in `.so` followed by zero or more
dot-decimal ABI components. Directory files with any executable permission bit
are also rejected where the filesystem exposes those bits. These checks bound
the declared package format; StreamView never executes a package file even when
arbitrary bytes use an innocuous suffix.

Version 1 applies all of these limits before installation:

- at most 1,024 files;
- at most 16 path components, 80 bytes per component, and 240 bytes per path;
- at most 8 MiB per file;
- at most 64 KiB for `rule.toml`;
- at most 64 MiB total uncompressed file data; and
- at most 64 MiB for an input `.svrule` container.

Limit arithmetic is checked. An archive's declared sizes do not authorize an
allocation or extraction beyond these bounds.

## Content Hash And Identity

The content hash is SHA-256 over the validated logical tree, not over directory
metadata or ZIP bytes. Files are ordered by their case-sensitive ASCII path
bytes. The hash input is:

```text
ASCII("StreamViewRulePackage\0")
u32be(1)
u32be(file_count)
for each file:
    u32be(path_byte_count)
    path_ascii_bytes
    u64be(file_byte_count)
    exact_file_bytes
```

Permissions, timestamps, archive offsets, and the source directory name are not
hashed. The displayed digest is `sha256:` followed by 64 lowercase hexadecimal
characters.

A package identity is the tuple `(package.id, package.version, content hash)`.
An entry-point identity adds its manifest `entrypoints.id`. Saved sessions,
cache namespaces, and diagnostics must retain the complete tuple; a path,
package ID, or version alone never proves reusable identity.

## Catalog Semantics

The catalog keeps different semantic versions of one package ID side by side.
It does not silently replace or automatically update a version. Registering an
already known complete identity is idempotent. Registering the same package ID
and textual version with another content hash is an explicit version conflict
and leaves the existing catalog unchanged. Persistent registration is atomic
under a uniqueness constraint on `(package.id, package.version)`; concurrent
registrations either observe the same hash or one receives the conflict.

Catalog lookup for analysis is exact. It first resolves package ID, version,
and content hash, then the entry-point ID, then checks the language contract and
current engine range. If the requested hash is absent while the same package ID
and version names another installed hash, lookup reports `version-conflict`; if
no such version slot exists, it reports `missing-content`. An unknown entry
point or incompatible language/engine version has its own diagnostic. Selection
never falls forward to another installed version. Recommendation and user-facing
sorting use SemVer 2.0.0 precedence but do not alter exact session pinning.

## Deterministic `.svrule` Container

A version 1 `.svrule` is a canonical single-disk ZIP32 archive using only
stored, uncompressed regular-file entries. Store-only encoding keeps package
reading independent of a compression library and removes decompression-ratio
ambiguity from the zip-bomb boundary.

The canonical writer emits validated files in the same path order used by the
content hash and fixes every representational choice:

- one local header and one central-directory record per file;
- local signature `0x04034b50`, version-needed `20`, flags `0`, method `0`,
  matching CRC-32 and 32-bit sizes, filename length equal to the canonical path,
  and extra length `0`;
- DOS time `00:00:00` and date `1980-01-01`;
- no data descriptors, encryption, ZIP64, extra fields, directory entries,
  per-entry comments, archive comments, or trailing bytes;
- central signature `0x02014b50`, version-made-by `0x0314` (Unix, ZIP 2.0),
  version-needed `20`, the same flags, method, time, date, CRC, sizes, and name,
  extra/comment lengths `0`, start disk `0`, internal attributes `0`, external
  attributes `(0100444 << 16)`, and the exact local-header offset; and
- end signature `0x06054b50`, disk fields `0`, equal record counts, the exact
  central-directory size and offset, and comment length `0`.

ZIP64 sentinel counts, sizes, and offsets are invalid. The importer parses from
byte zero in emitted order: every local header begins at the current offset and
its file data follows its name immediately; the central directory begins after
the final data byte; every central record points to the corresponding recorded
local offset; and the end record begins immediately after the central directory
and ends at the container length. Local and central fields must agree, names and
data ranges cannot overlap or alias, and CRC-32 values must match. All offset,
length, count, and cumulative-size arithmetic uses checked at-least-64-bit
operations and must fit both the actual container and version 1 limits before
allocation or seek. A ZIP that is safe but not canonical is rejected and can be
unpacked by another tool then repacked by StreamView. The canonical byte
sequence is deterministic across supported platforms; repeated packaging of
the same tree must be byte-for-byte identical.

## Content-Addressed Installation

Validated content is installed below
`sha256/<first-two-digest-characters>/<complete-digest>/` in the configured
rule store. The installer creates an unpredictable sibling staging directory
exclusively on the same filesystem, writes only copied bytes, verifies the
staged logical tree and hash, makes files read-only, and atomically renames the
staging directory into an absent destination. It never merges with or replaces
an existing destination. Concurrent installers either create the destination
once or revalidate the winner. Stale private staging directories contain no
cataloged package and may be removed.

An existing destination is accepted only after its tree revalidates to the same
digest with the same no-link, no-reparse, path, type, size, manifest, and hash
rules. Corruption is reported rather than repaired or silently replaced. Store
ownership must prevent untrusted writers from mutating a tree between this
validation and catalog use.

The content-addressed directory is immutable package data. Catalog metadata may
refer to it, and unused hashes may later be garbage-collected only after no
saved session or installed catalog record pins them.
