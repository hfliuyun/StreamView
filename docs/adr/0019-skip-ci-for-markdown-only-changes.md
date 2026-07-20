# Skip CI For Markdown-Only Changes

Status: Accepted
Date: 2026-07-20

## Context

The hosted matrix builds and tests three desktop environments. A commit that
changes only Markdown documentation cannot change the C++ targets, Qt inputs,
rules, tests, or packaged binaries, but it previously still consumed the full
matrix. The implementation plan and the bilingual technical documents are
updated frequently, so this creates avoidable queue time and runner usage.

## Decision

- Configure both `push` and `pull_request` triggers with `paths-ignore: '**/*.md'`.
- A workflow is skipped only when every changed path ends in `.md`.
- A commit that changes any source, build, workflow, configuration, test, or
  package file still runs the complete matrix, even when Markdown files are
  changed in the same commit.
- Markdown-only changes are checked by review and local `git diff --check`; no
  executable behavior is expected from them.

## Consequences

Plan updates and documentation-only corrections no longer start redundant
Build/Test/Install jobs. The workflow does not validate Markdown syntax, and a
future documentation tool must be added explicitly if that becomes necessary.
Changing the workflow itself remains a non-Markdown change and therefore runs
the matrix.
