# Portable Cancellation State

Status: Accepted
Date: 2026-07-19

## Context

Phase 1 initially implemented `CancellationSource` and `CancellationToken` as
thin wrappers around the C++20 `std::stop_source` and `std::stop_token` types.
The implementation built with the local Apple Clang 21 toolchain and with the
hosted GCC and MSVC toolchains. The macOS 15 ARM64 runner for hosted run
`29693108274`, however, compiled in C++20 mode with an Apple libc++ that did not
provide either type. This caused the cancellation test to fail at compile time.

The C++ language mode alone does not guarantee that every C++20 library feature
is present in all supported standard-library versions. Cancellation in
StreamView needs only a copyable polling token and an idempotent, thread-safe
request operation; it does not currently need stop callbacks or integration
with `std::jthread`.

## Decision

- Preserve the public `CancellationSource` and `CancellationToken` API and its
  observable behavior.
- Back source-issued tokens with a reference-counted shared state containing an
  atomic cancellation flag.
- Keep cancellation requests thread-safe, non-throwing, and idempotent.
- Keep the state alive as long as any source or token references it.
- Do not expose the storage mechanism as part of the normative analysis model.
- Reconsider the standard stop-token facilities only when every supported
  toolchain provides them and the runtime needs callbacks or `std::jthread`
  integration.

## Consequences

The cancellation API compiles consistently on the fixed Windows, macOS, and
Ubuntu CI toolchains without reducing the C++20 project baseline. StreamView
owns a small amount of synchronization code and one shared allocation per
cancellation state. Polling remains explicit, so later VM work must still check
the token at the documented instruction interval.
