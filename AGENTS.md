# StreamView Agent Instructions

Read `GEMINI.md` for the project-wide development protocol.

When a task adds or materially changes a media format, read
`docs/format-extension-contract.md` in full before editing. That contract is
the completion gate: bilingual ADR, normative format documentation,
deterministic fixtures, tests, local dev/ci/sanitize verification, affected
Hosted CI, and an `implementation-plan.md` record are all required.

Keep format semantics in official rule packages and `src/rules/`. Change
`src/app/` only for format-neutral integration and presentation. Change
`src/core/` or the DSL only under a separately scoped capability ADR backed by
a scratch probe. If the existing contract cannot express the format, stop and
report the original probe error.
