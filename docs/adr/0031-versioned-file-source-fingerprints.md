# Versioned File Source Fingerprints

Status: Accepted
Date: 2026-07-29

## Context

Saved locations and rebuildable analysis pages must never be rebound using only
`RandomAccessSource::identity()`. For `FileSource` that identity is path-like, so
the same string can name replaced content. Full hashing is precise for small
files but conflicts with bounded initial work for 100 GB media. M5 therefore
needs one explicit, versioned file-fingerprint contract before cache namespaces
or `.svsession` can authorize reuse.

The implementation must fingerprint the file that is already open for analysis.
Reopening its path could hash a replacement while the session still reads the
old handle. Metadata and contents may also change during computation, which
must be diagnosed rather than serialized as a coherent identity.

## Decision

`SourceFingerprint` version 1 is a validated core value with an algorithm
version, closed mode, file size, optional nanosecond modification time, and one
32-byte SHA-256 digest. It never contains a filesystem path or
`RandomAccessSource::identity()`.

`FileSource::fingerprint()` operates on the same read-only OS handle and under
the same seek/read mutex as analysis reads. It captures file size and
modification time before and after hashing. A size different from the size
recorded at open, or any before/after snapshot difference, reports
`source-changed`; unsupported handle metadata and I/O errors remain distinct.

Version 1 uses two modes:

- Files up to and including 3 MiB use `full-content-sha256`. The digest is
  SHA-256 over every file byte. The durable value includes version, mode, size,
  and digest; modification time is absent, so touching unchanged small content
  does not invalidate a session.
- Larger files use `sampled-sha256`. The durable value includes version, mode,
  size, Unix-epoch modification time in nanoseconds, and SHA-256 over the
  concatenation of three 1 MiB windows. The windows start at byte 0,
  `floor((size - 1 MiB) / 2)`, and `size - 1 MiB`.

The 3 MiB boundary ensures full hashing never reads more payload than the three
large-file samples. Hashing uses a 64 KiB working buffer, so memory use is
independent of source size. POSIX timestamps come from `fstat` nanosecond
fields; Windows `FILETIME` is converted from 100 ns ticks to Unix-epoch
nanoseconds. Values with an unsupported version, wrong mode/size/mtime shape,
or non-SHA-256 digest are rejected at construction.

Only local regular `FileSource` instances receive this automatic contract.
Virtual or future remote sources must define an explicit trustworthy
fingerprint policy; they never fall back to caller-defined identity text.

## Consequences

Small files receive content-exact durable identity without metadata-only false
mismatches. Large-file work remains bounded to 3 MiB plus two metadata
snapshots, while size and nanosecond mtime cover changes outside sampled
windows according to the accepted product algorithm. A sampled fingerprint is
change detection, not a cryptographic commitment to every byte.

Cache namespace framing and `.svsession` serialization can now consume one
validated value and its algorithm version. They must still bind exact rule
package and entry-point identity plus their own schema and payload versions;
this decision alone does not authorize persistent cache reuse.

## Considered Options

- Use path, size, and mtime only: replacement content can preserve all three.
- Hash every file: makes opening or restoring very large media depend on a full
  sequential read.
- Store mtime for small files: invalidates content-exact fingerprints after a
  metadata-only touch.
- Reopen the path for hashing: can fingerprint a different file from the open
  analysis handle.
- Add `fingerprint()` to every `RandomAccessSource`: permits virtual sources to
  invent durable identity without an explicit provenance contract.
