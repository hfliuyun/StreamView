# Lazy Analysis For Large Files

The analysis session will use random access, lazy parsing, and cancellable background indexing so files of at least 100 GB can be inspected without memory usage growing with the source size. The initial view must become useful quickly, while deeper structure is materialized on demand and retained as incremental results.

**Consequences**

- Parsers must expose resumable, bounded work rather than only returning a complete tree.
- UI state must distinguish not-yet-indexed data from unsupported syntax and parse errors.
- Jumping to a known source offset must not require waiting for a complete-file scan.
