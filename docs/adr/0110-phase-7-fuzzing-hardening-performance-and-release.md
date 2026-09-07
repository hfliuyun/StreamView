# ADR-0110: Phase 7 Security Hardening, Fuzzing Strategy, Performance Baselines, and Release Automation

- **Status**: Proposed
- **Date**: 2026-09-07
- **Authors**: StreamView Contributors

---

## Context

With the successful completion and milestone clearance of Phase 6 (Session Lifecycle, Pinned Rule Overrides, Rule Package Management, and Desktop User Experience), StreamView v0.1 has assembled its complete functional surface across core models, declarative DSL engines, official media rule packages (H.264 Annex B, AAC ADTS/ASC, ISO BMFF MP4/MOV), and the Qt-based GUI application.

Phase 7 represents the culminating phase of StreamView v0.1: **Security, Performance, and Release**. Its mission is to rigorously harden the entire software supply chain and runtime execution pipeline against malicious inputs, verify performance baselines against giant media files, automate multi-platform release artifact generation, and fulfill all legal and licensing requirements for public release.

### Milestone Clearance Constraints and Immediate Preconditions

The independent milestone review of Phase 6 cleared the milestone gate while attaching two immediate, mandatory preconditions that must be resolved within Task P7a before launching the broader fuzzing and release pipeline:

1. **P1-1-R1 (Localization Guard Multiline Concatenation Blindness)**:
   - The test scanner in `tests/app/theme_localization_test.cpp` relied on `\btr\(\s*"((?:[^"\\]|\\.)*)"\s*\)`, which terminated at the first closing quote followed by `)`. Consequently, C++ multiline string literal concatenations (e.g., `tr("part1 " "part2")`) were invisible to the guard.
   - Two multiline UI dialog strings in `format_override_dialog.cpp` and `rule_manager_dialog.cpp` were omitted from `LocalizationManager` without failing the guard.
   - Requirement: Generalize the scanner to parse `tr(` until the balancing `)` and concatenate all enclosed string literals. Enforce two-stage verification: first prove the test turns RED when encountering unmapped concatenated literals, then update the dictionary to restore a strict 100% equality match.

2. **P1-2-R1 (Sibling Disambiguation Mutation Testing Proof)**:
   - In `tests/app/main_window_test.cpp`, the H.264 SPS test asserted node restoration on `offset_for_ref_frame[1]`. However, the DSL engine automatically appends `[i]` to repeated field names (`offset_for_ref_frame[0]` vs `offset_for_ref_frame[1]`), meaning their display strings were already distinct.
   - Requirement: Ground the sibling disambiguation guarantee by conducting an explicit mutation verification (e.g., demonstrating that removing `#<row>` disambiguation causes restoration failure on identical sibling names or synthetic multi-field collisions).

3. **Performance Baseline Inquiries**:
   - `MainWindow::currentUserState()` performs recursive full-tree traversal (`collectExpanded`) on every save.
   - `MainWindow::openSessionFile()` triggers path-based expansion (`expandNodeByPath`) and selection restoration while asynchronous analysis may still be incomplete on multi-batch files.
   - Requirement: Formally integrate both execution paths into the Phase 7 benchmark harness for continuous latency and memory profiling.

---

## Decisions

### 1. Fuzzing Architecture and Execution Strategy

**Decision**: Implement a modular, multi-target fuzzing harness covering all untrusted data ingestion boundaries.

#### 1.1 Fuzz Targets Matrix
StreamView exposes six distinct trust boundaries where untrusted or malformed binary streams interact with parsing logic:

| Target Name | Component Under Test | Ingestion Interface | Threat Model & Invariants |
| :--- | :--- | :--- | :--- |
| `fuzz_dsl_parser` | `DslParser` | Raw `.svfmt` text | Reject malformed grammar without recursion depth explosion or unhandled exceptions. |
| `fuzz_dsl_compiler` | `DslCompiler` | Valid syntax AST | Reject semantic/type errors without panics; ensure deterministic IR generation. |
| `fuzz_dsl_vm` | `DslVirtualMachine` / `DslExecutor` | Compiled bytecode + synthetic bitstream | Bounded loop execution; strict budget enforcement; zero out-of-bounds bit reads. |
| `fuzz_rule_package_store` | `RulePackageStore` | Raw `.svrule` (ZIP archive + TOML) | Defense against Zip Slip (directory traversal), zip bombs (uncompressed ratio > 100x), and corrupt manifests. |
| `fuzz_format_detectors` | Annex B, ADTS, and MP4 Box Detectors | Raw binary file prefix (up to 4 MiB) | Never crash or hang on arbitrary byte streams; deterministic arbitration output. |
| `fuzz_mp4_sample_extractor` | `Mp4SampleTableExtractor` | Synthetic `stbl` box byte blobs | Integer overflow prevention during chunk/sample index math; bounded memory allocation. |

#### 1.2 Harness Implementation & Portability
- **Engine**: Build targets supporting LLVM `libFuzzer` (`-fsanitize=fuzzer,address,undefined`) when built with Clang.
- **Standalone Mode**: Provide an embedded standalone driver (`main()` entry point) for every fuzzer target to enable deterministic execution of corpus test cases under CTest across all three CI platforms (Ubuntu, macOS, Windows) without requiring `libFuzzer` runtime libraries.
- **Corpus Management**: Seed corpora derived from existing fixture suites (`tests/fixtures/`) augmented with known edge cases (zero-length streams, max-integer headers, truncated NAL units, cyclic box hierarchies).
- **Path Disambiguation Corpus Target (Task P7c)**: In Task P7c, rule package and format scanner fuzzing corpora will explicitly incorporate synthetic syntax trees whose field and node names contain `#<digits>` (e.g., `tag#1`, `entry#0`) to verify path resolution robustness and confirm zero collision with `#<row>` sibling disambiguation.

---

### 2. Full-Matrix Hardening & Static Analysis

**Decision**: Enforce compiler warning elevation, AddressSanitizer/UndefinedBehaviorSanitizer clean runs, and automated static linting across the entire build matrix.

1. **Compiler Warning Standards**:
   - GCC / Clang: `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` (on project targets).
   - MSVC: `/W4 /WX /permissive-` with structured exception handling safety.
2. **Sanitizer Zero-Tolerance**:
   - `cmake --preset sanitize` (ASan + UBSan) must execute all 53+ test suites with 0 reports, 0 memory leaks, and 0 undefined behavior detections.
   - Windows AddressSanitizer (`/fsanitize=address`) evaluated for CI validation.
3. **Automated Static Analysis**:
   - Clang-Tidy integration via CMake `CMAKE_CXX_CLANG_TIDY` checking:
     - `bugprone-*`: narrowing conversions, unhandled return values, use-after-move.
     - `modernize-*`: effective modern C++20 patterns.
     - `performance-*`: unnecessary copies, redundant string allocations.
     - `clang-analyzer-*`: dead code, null pointer dereferences, uninitialized memory.

---

### 3. Performance Baselines and Diagnostic Observation Harness

**Decision**: Establish deterministic, non-flaky performance regression benchmarks with enforced budget gates.

#### 3.1 Hard Performance Budgets

| Metric | Budget Threshold | Measurement Scenario | Target Platform |
| :--- | :--- | :--- | :--- |
| **Initial View Availability** | $\le 2.0\text{ s}$ | App launch to interactive root tree rendering on typical media (`tests/fixtures/mp4_p5j4_avc_multi_nal.mp4`). | dev / ci / sanitize |
| **100 GB Virtual Sparse Source Memory** | $\text{RSS} \le 512\text{ MiB}$ | Full session open, fingerprint calculation, root scanning, and tree rendering of 100 GB sparse source. | macOS / Linux / Windows |
| **Known-Offset Page Read Latency** | $p95 < 100\text{ ms}$ | Random-access 64 KiB page read via `SourcePager` across 1,000 queries. | dev / ci |

#### 3.2 Targeted Observation Paths
1. **`collectExpanded` Traversal Profiling**:
   Benchmark `MainWindow::currentUserState()` under deep trees ($\ge 10,000$ materialized nodes). Assert total serialization time does not exceed $50\text{ ms}$.
2. **Asynchronous Restoration Profiling**:
   Benchmark `MainWindow::openSessionFile()` on sources requiring multi-batch analysis. Verify that view state restoration operates deterministically without blocking UI responsiveness.

---

### 4. Release Packaging Automation, Licensing & Compliance

**Decision**: Automate reproducible, verifiable release packages across Windows, macOS, and Linux, equipped with complete licensing attributions and integrity digests.

#### 4.1 Packaging Matrix

| Target Operating System | Architecture | Package Format | Packaging Mechanism | Contents |
| :--- | :--- | :--- | :--- | :--- |
| **Windows** | x64 | `.zip` | CMake `CPack` / `windeployqt` | `streamview.exe`, `svtool.exe`, Qt runtime DLLs, official rule packages, licenses. |
| **macOS** | ARM64 (Apple Silicon) | `.app.zip` | `macdeployqt` | Standalone bundle `StreamView.app` with embedded Qt frameworks and bundled rules. |
| **Linux** | x86_64 | `AppImage` | `linuxdeployqt` / AppImageKit | Self-contained runnable executable for glibc 2.35+ distributions (Ubuntu 22.04+). |

#### 4.2 Release Integrity & Compliance Artifacts
Every release bundle must include:
1. `SHA256SUMS.txt`: Cryptographic SHA-256 digests of all distributed archives.
2. `SBOM.json`: Software Bill of Materials in standard SPDX 2.3 JSON format enumerating dependencies (Qt, SQLite, zlib).
3. `LICENSE`: StreamView MIT License terms.
4. `LICENSES/`: Granular license texts for Qt (LGPLv3), SQLite (Public Domain), and bundled assets.
5. `README_QT_SOURCE.txt`: Explicit written offer and instructions for obtaining source code of LGPLv3-licensed Qt libraries used in binary distributions.

#### 4.3 Staged Release Pipeline
Releases proceed strictly through sequential git tags:
1. `v0.1.0-alpha`: Fuzzing and static analysis completed; packaging workflows verified.
2. `v0.1.0-beta`: Performance baselines verified; community preview.
3. `v0.1.0-rc`: Release candidate; final documentation and licensing audit.
4. `v0.1.0`: Official general availability release.

---

### 5. Analysis Path Disambiguation and Restoration Semantics

**Decision**: Formally document and enforce the three-tier node resolution semantics in `MainWindow::findIndexByPath(const QString& path)`:
1. **Tier 1 (Sibling Index Disambiguation)**: If a path token contains `#<row>` where `<row>` is one or more ASCII digits at `lastIndexOf('#') > 0`, attempt to match the child node at row `<row>` whose name strictly matches the prefix before `#`.
2. **Tier 2 (Exact Literal Fallback)**: If Tier 1 fails to match (e.g., the node is not at row `<row>`, or `<row>` is out of bounds, or the node is literally named `prefix#<digits>`), fall back to scanning all children for an exact literal name match where `data(Qt::DisplayRole).toString() == part`. This guarantees that nodes legitimately named with `#<digits>` remain fully addressable.
3. **Tier 3 (Legacy Row Number Fallback)**: If Tier 2 fails and the entire token is numeric, treat it as a direct row index for backward compatibility with legacy test fixtures.

Furthermore, state restoration lifecycle guarantees are hardened:
- **Navigation Isolation**: Pending tree restoration (`applyPendingTreeState()`) is strictly prohibited from executing when `session_->navigationDepth() != 0`, preventing root paths from corrupting child/sample trees (P2-43).
- **Format Override Reset**: Overriding format (`MainWindow::overrideFormat()`) unconditionally clears pending selection and expanded paths to prevent cross-format path collision (P2-44).
- **Interactive Preemption**: Any manual user selection in `analysisTreeView` while streaming analysis is in progress immediately clears `pendingSelectedAnalysisPath_`, preventing asynchronous batch arrivals from stealing the user's cursor (P2-45).

---

## Phase 7 Work Breakdown Structure (WBS)

- [ ] **Task P7a** (Specification & Review Remediations): Author bilingual ADR-0110; resolve P1-1-R1 (red-then-green multiline string localization guard); resolve P1-2-R1 (sibling disambiguation mutation proof); update Phase 7 WBS (Fixed Independent Review Gate).
- [ ] **Task P7b** (Fuzzing Harness & Core Targets): Implement fuzzing drivers for DSL parser, compiler, and VM; connect standalone fuzz runners to CTest.
- [ ] **Task P7c** (Rule Package & Format Scanner Fuzzing): Implement fuzzing targets for `RulePackageStore` (ZIP/manifest), H.264, AAC, and MP4 box/sample scanners.
- [ ] **Task P7d** (Static Analysis & Compiler Warning Elevation): Configure Clang-Tidy checks and elevated warning compiler flags across dev/ci presets.
- [ ] **Task P7e** (Performance Benchmarks & Profiling Harness): Implement performance regression tests for initial view latency, 100 GB sparse memory RSS, and page read latency; profile `collectExpanded` and multi-batch restoration.
- [ ] **Task P7f** (Windows Packaging Automation): Configure CPack and `windeployqt` for Windows x64 standalone ZIP.
- [ ] **Task P7g** (macOS Packaging Automation): Configure `macdeployqt` for macOS ARM64 `.app.zip`.
- [ ] **Task P7h** (Linux Packaging Automation & Packaging Gate): Configure AppImage generation for Linux x86_64; complete licensing/SBOM audit (Fixed Independent Review Gate).
- [ ] **Task P7i** (Pre-Release Staging & `v0.1.0-alpha`): Tag and generate alpha release assets.
- [ ] **Task P7j** (Validation & `v0.1.0-beta`): Validate beta candidate across target OS environments.
- [ ] **Task P7k** (Release Candidate & `v0.1.0-rc`): Final sanity checks, documentation freeze, and RC tagging.
- [ ] **Task P7l** (Final Release Gate & `v0.1.0` GA): Comprehensive release signoff and general availability release (Fixed Independent Review Gate).

---

## Review Items Addressed

- **P1-1-R1**: Multiline concatenated string literal localization guard and strict dictionary mapping (red-then-green verified in `tests/app/theme_localization_test.cpp:140-164`, commit `6e8f9a0`).
- **P1-2-R1**: Sibling disambiguation mutation testing proof (verified with inverted `#<row>` deletion failure test in `tests/app/main_window_test.cpp:2248-2281`, commit `6e8f9a0`).
- **P2-36**: Aligning review item status definitions between documentation and review reports.
- **P2-37**: Synchronizing historical task status in `docs/implementation-plan.md:253` ("P2-19 缓解").
- **P2-38**: Standardizing test count reporting metrics to "N test slots (QTest totals M)".
- **P2-39**: Correcting formal document citation links to `docs/adr/0019-skip-ci-for-markdown-only-changes.md`.
- **P2-40**: Documenting `#<row>` boundary resolution semantics and scheduling `#<digits>` node name fuzzing corpus for Task P7c.
- **P2-43**: Guarding terminal `advanceAnalysis()` pending tree restoration with `rootTreeIsActive` (`src/app/main_window.cpp:1092-1096`).
- **P2-44**: Resetting pending tree selection and expansion states upon format override in `MainWindow::overrideFormat()` (`src/app/main_window.cpp:820-821`).
- **P2-45**: Preempting pending restored selection when user manually interacts with analysis tree during streaming analysis (`src/app/main_window.cpp:307-309`).
- **P2-46**: Wrapping built-in format options in `FormatOverrideDialog` with `tr()`, expanding dictionary to 154 keys, and upgrading `ThemeLocalizationTest` assertions to 154 keys and 227 calls (`src/app/localization_manager.cpp:35-43`, `tests/app/theme_localization_test.cpp:170-171`).

## Deferred Review Items

- **P2-41**: Formal release build execution time profiling and performance regression harness is deferred to Task P7e (performance baselines), rather than using uncalibrated CI runner wall-clock times.
