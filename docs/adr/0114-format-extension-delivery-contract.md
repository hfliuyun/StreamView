# ADR-0114: Format Extension Delivery Contract

- Status: Accepted
- Date: 2026-10-13
- Authors: StreamView contributors

## Context

StreamView now has a stable DSL, official rule packages, format-neutral core
models, cross-platform tests, and a release workflow. Future work will
occasionally add formats such as WAV. A format can appear to work while still
having an undocumented normative subset, weak detection boundaries, missing
truncation semantics, or no evidence that large inputs remain bounded.

The project also has a strict rule that architecture decisions are written
before implementation and that English technical documents are normative.
That rule needs one explicit delivery contract for future format additions so
an agent cannot complete only the parser or only the UI and call the format
finished.

## Decision

Adopt [`docs/format-extension-contract.md`](../format-extension-contract.md)
as the normative process and boundary contract for every new or materially
changed media format. Maintain the Chinese companion at
`docs/zh-CN/format-extension-contract.md`.

Every format task must:

1. classify the format and its framing before coding;
2. add a consecutive bilingual ADR before implementation;
3. define package identity, normative references, supported subset,
   postponements, detection evidence, source spans, diagnostics, limits,
   cancellation, and version consequences;
4. keep format semantics in an official DSL rule package and `src/rules/`;
5. change `src/app/` only for format-neutral integration and presentation;
6. change `src/core/` or the DSL only through a separately scoped capability
   decision supported by a scratch probe;
7. add deterministic fixtures, targeted tests, and relevant fuzz coverage;
8. run the local dev/ci/sanitize matrix, affected Hosted CI, and
   `git diff --check`; and
9. record actual evidence in `docs/implementation-plan.md` before acceptance.

Any change to a rule package's decoding output or manifest, including `depth`,
requires a package version bump. Rule packages remain data-only and cannot
gain native code, process/network access, symlinks, or runtime dependency
resolution.

## Consequences

The contract adds documentation and verification work to each format task, but
it makes the supported subset, failure behavior, and ownership boundaries
reviewable. A WAV implementation, for example, must first decide whether it
covers RIFF/WAVE PCM only, WAVE extensible, RF64, or compressed variants; those
choices become an ADR and package documentation rather than accidental
behavior.

The contract does not require a new scanner or a core change when the existing
DSL and rule runner can express the format. Conversely, an agent must stop and
report a probe failure instead of placing format-specific parsing in the core.

## Verification

This ADR is documentation-only. Acceptance requires:

- the English and Chinese contract documents;
- a root agent pointer and the existing AGY rules pointing to the contract;
- links from both documentation indexes; and
- a passing Markdown hygiene test and `git diff --check`.

Future format ADRs must reference ADR-0114 and the format-extension contract.
