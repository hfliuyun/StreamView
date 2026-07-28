# Cache Rebuildable Pages In SQLite WAL

Status: Accepted
Date: 2026-07-28

## Context

ADR-0004 requires bounded, resumable analysis for sources of at least 100 GB.
The source reader, progressive H.264 scanner, mapper, and analysis publisher now
perform bounded work, but rebuildable index and materialized-result data still
has no disk-backed cache. The implementation plan assigns SQLite WAL, schema
versioning, batched commits, and crash recovery to the remaining M4 cache
slice.

The durable identity needed to reuse cached analysis across sessions is not yet
available. `RandomAccessSource::identity()` is path-like for files and
caller-defined for virtual sources. ADR-0027 therefore rejects persistent
analyzer recovery until a source fingerprint, exact rule-package identity and
content hash, cache namespace, schema version, and durable analyzer state can
be validated together. Those identity contracts remain M5 work.

The current H.264 analyzer also has private scanner, mapper, queue, compiled
program, and identifier-allocation state. Its analysis tree has no arbitrary
snapshot import operation. A cache can safely store committed rebuildable data
now, but it cannot honestly promise to restore a live analyzer or resume it
across processes.

## Decision

Core exposes a thread-affine `PagedCache` module. Its interface is expressed in
cache namespaces, opaque page keys, page bytes, reads, and atomic batch commits.
Qt SQL connections, SQLite statements, table names, PRAGMAs, schema bootstrap,
integrity checks, transaction ordering, and recovery markers remain private to
the implementation.

The module stores two closed page kinds:

- progressive-index pages; and
- materialized-result pages.

It does not cache media-source bytes. `RandomAccessSource` remains the source of
truth, and `SourcePager` continues to retain at most the one 64 KiB page returned
to its caller. Index and materialization owners define the encoding inside each
cache page. The cache does not interpret analysis nodes, source mappings,
format contexts, or rule-owned payloads.

Opening a cache requires a database path and a non-empty caller-supplied
namespace. In M4 the namespace is an opaque partition for rebuildable data, not
a source fingerprint or proof that data may be rebound to another analysis
session. Production reuse across session or process boundaries is forbidden
until M5 constructs the namespace from the source fingerprint, exact
rule-package identity and content hash, schema/cache namespace, and the
relevant payload-format versions.

A page key consists of its closed page kind, a caller-owned stream identifier,
and a logical page index. Stream identifiers and page indexes must fit SQLite's
non-negative signed 64-bit integer range. A payload contains 1 through 64 KiB.
One commit contains 1 through 256 pages and no duplicate key. These limits keep
both validation and one transaction bounded to at most 16 MiB of caller data.

`commitBatch` is the only write operation. It validates the complete batch
before touching the database. On success every page is visible together;
failure exposes none of the batch. Committing an existing key atomically
replaces that cache page, which permits a partially filled final index page to
be rewritten without exposing an intermediate encoding. Reads return found,
missing, invalid-request, thread-violation, or storage-error outcomes. A cache
miss is normal rebuildable absence, not a storage failure.

Each `PagedCache` instance and its SQLite connection are created, used, and
destroyed on one thread. Wrong-thread reads and commits are rejected before SQL
is touched; wrong-thread destruction is a fatal contract violation because Qt
cannot safely remove the owning thread's connection. The first implementation
does not expose a connection pool or background queue. A future worker creates
and owns its cache instance in that worker thread.

One live `PagedCache` owns one database path exclusively through a cross-process
lock file. Another open of that path fails while the owner is alive. This keeps
schema initialization, recovery, and the marker protocol single-writer even
when different namespaces share one file. A stale lock from a terminated
same-host process is removed using its recorded process identity before crash
recovery; a live lock is never removed merely because of its age. Future
concurrent readers or writers require a separate connection-ownership and
recovery-lock decision.

The SQLite file uses a fixed StreamView `application_id` and schema version 1
through `user_version`. A new empty database is initialized transactionally.
An existing database with another application ID, an older or newer unsupported
schema, missing required schema objects, or a failed `quick_check` is rejected
without repair or reuse. No migration is implied by version 1; a future schema
change must define an explicit migration or invalidation decision first.

Every opened connection enables and verifies:

- `journal_mode=WAL`;
- `foreign_keys=ON`;
- `synchronous=NORMAL`; and
- a 5,000 ms busy timeout.

The QSQLITE driver is a runtime requirement. Cache open reports a distinct
missing-driver result before creating a connection, and the core test suite
checks driver availability on every supported build platform. The core target
links Qt Sql privately so no `QSql*` type enters the public header.

Process-crash recovery combines SQLite transactions with an explicit pending
marker. While holding exclusive ownership of the database path, `commitBatch`
records one persistent marker for that namespace in a separate autocommit step.
It then starts an immediate transaction, writes every page, deletes the marker,
and commits. Therefore:

- a crash before or during the transaction leaves the marker, while SQLite
  rolls back every uncommitted page change;
- a successful transaction makes all pages visible and removes the marker in
  the same commit; and
- opening the database deletes abandoned markers transactionally and reports
  how many were recovered.

If abandoned-marker cleanup cannot be committed, open fails with a storage
error and exposes no cache instance. With `synchronous=NORMAL`, this is a
process-crash recovery and atomicity contract, not a guarantee that the marker
or cache pages survive sudden power loss. Cache data remains rebuildable.

The marker is recovery evidence, not a serializable analyzer checkpoint. Page
payloads from completed commits remain rebuildable cache data and may be
discarded whenever their namespace cannot be validated.

The recovered count includes a marked batch attempt whose cleanup did not
finish, normally because the process terminated. An ordinary commit error
performs best-effort marker cleanup before returning and is not counted when
that cleanup succeeds. A recovered marker does not imply that page writes had
begun.

## Consequences

Callers get a small interface for bounded reads and atomic page publication,
while SQLite connection lifetime, WAL behavior, schema validation, and crash
cleanup remain local to one implementation. A caller cannot accidentally hold
an SQL transaction open, forget to roll it back, or depend on internal tables.

The 64 KiB page and 256-page batch limits bound one call independently of
source size. The cache retains no page payload in memory after a call returns;
caller-owned page and analysis-node LRU policy remains separate.

This slice establishes persistent storage mechanics but not persistent analyzer
recovery. M5 must provide a source fingerprint and exact rule identity before
an existing namespace can be trusted. Restoring scanner, mapper, context
directory, analysis tree, or logical-view allocator state requires additional
versioned serializers and remains separate work.

## Considered Options

- Expose `begin`, `put`, `commit`, and `rollback`: makes callers manage SQLite
  ordering and abandoned transactions even though every current caller needs
  the same atomic batch behavior.
- Cache pages under `RandomAccessSource::identity()`: silently reuses stale data
  when a path is replaced and contradicts ADR-0027.
- Serialize the live H.264 analyzer now: requires unimplemented source/rule
  identity, tree import, scanner/mapper checkpoints, and context payload
  versioning.
- Cache source bytes alongside index pages: duplicates read-only media data and
  conflicts with the existing bounded random-access source design.
- Use one shared cross-thread `QSqlDatabase`: violates Qt SQL connection
  ownership and adds concurrency policy before a background cache owner exists.
- Rely only on SQLite's rollback without a marker: preserves atomic pages but
  gives cache open no explicit, testable evidence that an interrupted batch was
  found and recovered.
