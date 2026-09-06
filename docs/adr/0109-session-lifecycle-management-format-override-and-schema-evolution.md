# ADR-0109: Session Lifecycle Management, Format Manual Override, and Schema Evolution Policy

- **Status**: Proposed
- **Date**: 2026-09-06
- **Authors**: StreamView Contributors

---

## Context

StreamView Phase 5 delivered complete non-fragmented ISO BMFF MP4/MOV container inspection, metadata tree materialization, lazy `mdat` encapsulation, windowed sample table indexing, and cross-layer sample navigation from container tracks down to elementary H.264/AAC bitstreams.

With the core container and elementary format engines in place, Phase 6 focuses on desktop session lifecycle, rule management, and desktop user experience:
1. **Desktop Session Lifecycle & Persistence**:
   - Dirty-state tracking (`isDirty()`), File > Save (`Ctrl+S`), File > Save As (`Ctrl+Shift+S`), and File > Open Session (`Ctrl+O` session variant).
   - Unsaved changes guardrails: intercepting window close, file switching, and application exit to prompt the user (Save / Discard / Cancel).
2. **Format Manual Override & Ambiguity Resolution**:
   - Resolving format detection ambiguities surfaced in Phase 5 (addressing review items P2-17 and P2-19): when auto-detection reports `formatSelection.ambiguous()` (e.g. concurrent container vs elementary stream evidence), users require an interactive UI path to resolve the ambiguity and select the intended format.
   - Allowing explicit format overrides: users may explicitly choose an installed/bundled rule entry point regardless of auto-detection outcome.
   - Architectural separation of pinned-rule execution vs detection arbitration (addressing review item P2-20).
3. **Rule Version Management**:
   - Visualizing installed and bundled rule packages, inspecting versions, content hashes, and entry points.
   - Importing and installing external `.svrule` packages via `RulePackageStore`.
4. **Desktop Experience Enhancements**:
   - Background analysis progress reporting and cancellation handling.
   - Global diagnostics dock summarizing warnings and errors with bidirectional navigation.
   - Theme switching (Light / Dark) and dynamic runtime bilingual localization (English / Simplified Chinese).

### Project Discipline Constraints & Specific Questions

1. **Schema Evolution Constraint (§4.3)**:
   - Does Phase 6 alter the `SessionDocument` schema, and if so, how are existing version 1 documents read?
   - Project discipline §4.3 strictly dictates: *"Do not write UI navigation stack into `SessionDocument` unless a new ADR explicitly defines its serialization semantics"*.
   - Project discipline §4.3 also dictates: *"Do not bundle capability and its first format consumer into the same commit"*.
2. **Historical Review Item Audit**:
   - **P2-17**: `formatSelection.ambiguous()` must be consumed as a trigger for format manual override in the UI.
   - **P2-18**: AAC ambiguity branches in detection arbitration represent whitebox candidate coverage and must be acknowledged.
   - **P2-19**: Pattern stream priority vs anchored detection disparity (`format_selection.cpp:145`) is cleanly resolved when manual format override supersedes heuristic stream-level tie-breakers.
   - **P2-20**: Cross-validation in tests uses `openPinnedMp4Fixture()`; in Phase 6, pinned rule construction becomes an explicit, supported feature path ("manual rule override"), separating pinned-rule tests from end-to-end detection arbitration tests.
   - **P2-21**: Single reference tool (`ffprobe`) limitation is recorded and accepted.

---

## Decision

### 1. `SessionDocument` Schema Invariance: Schema Version 1 Retained

**Decision**: The `SessionDocument` serialization schema **remains strictly at Version 1 (`schemaVersion: 1`)**. No schema version increment or schema structural modification shall be introduced in Phase 6.

**Architectural Rationale**:
1. **Format Manual Override is Already 100% Expressible in Schema Version 1**:
   In `SessionDocument` version 1 (defined in [ADR-0033](0033-save-exact-analysis-sessions-as-atomic-json.md) and [docs/session-format.md](../session-format.md)), the `rule` object contains:
   ```json
   "rule": {
       "packageId": "org.streamview.h264",
       "packageVersion": "0.1.0",
       "contentSha256": "3a7b...",
       "entryPointId": "h264_annex_b"
   }
   ```
   When a session is saved—whether the active rule was determined by auto-detection or explicitly chosen via manual override—it serializes the *active, bound `RuleEntryPointIdentity`*.
   Upon restore, ADR-0033 specifies exact catalog lookup by this `rule` identity and constructs the analyzer directly, **bypassing format auto-detection entirely**.
   Therefore, a saved session whose format was manually overridden already restores that exact format deterministically. Adding an extra `"formatOverride": true` boolean or schema field is redundant and unnecessary.
2. **UI Navigation Stack Remains Strictly Transient (Not Serialized)**:
   In accordance with ADR-0103 §6, ADR-0105 §6, and [src/app/analysis_session.h](../../src/app/analysis_session.h#L238):
   - Sample drill-down frames (`SampleNavigationFrame`) and child format frames (`NavigationFrame`) represent ephemeral interactive exploration overlays for the GUI, not persistent ground truth.
   - The root container format remains the authoritative document anchor.
   - Persisting child session execution contexts or sample navigation stacks into `.svsession` would leak rebuildable runtime state into what ADR-0033 intentionally defined as a compact record of immutable coordinates.
   - When a user saves a session while navigated inside a sample, the session document captures the root container's bookmarks, annotations, expanded paths, the current 64 KiB raw data page index, and the exact absolute source bit offset of the selection.
   - Upon restore, the root container tree is cleanly reconstructed, and the selected byte/bit offset is highlighted in `RawDataView`, allowing the user to immediately re-enter the sample via the timeline dock if desired.
3. **Flawless Backward Compatibility & Zero Migration Burden**:
   Retaining Schema Version 1 ensures that every existing `.svsession` file created since M5 remains fully readable without schema migrations, backwards-compatibility shims, or risks of document corruption.

---

### 2. Forward & Backward Compatibility and Schema Evolution Policy

1. **Strict Closed Schema Enforcement**:
   `SessionDocument::parse()` shall continue to enforce `hasExactKeys()` on the root and all nested objects:
   ```cpp
   hasExactKeys(root, {u"schemaVersion", u"source", u"rule", u"bookmarks",
                       u"annotations", u"expandedPaths", u"view"});
   ```
   Any `.svsession` file containing unknown, unexpected, or misspelled keys shall be rejected with `SessionDocumentLoadStatus::InvalidSchema`. This maintains deterministic replay and security boundaries.
2. **Governing Policy for Future Schema Revisions (Version N)**:
   If a future phase requires persisting new categories of durable user data (e.g. multi-source workspaces, cross-track alignment anchors):
   - **Mandatory Backward Compatibility**: The parser must inspect `schemaVersion` as the first token. If `schemaVersion == 1`, it must execute the immutable Version 1 parser. Older documents must NEVER be rejected due to application upgrades.
   - **No Dual-Write Complexity**: Until a new schema version is accepted by an explicit ADR, StreamView shall only write canonical Version 1 JSON.
   - **Migration Policy**: New versions must document an explicit translation mapping from Version 1 constructs to Version N constructs.

---

### 3. Session Lifecycle & Dirty-State Tracking Contract

`AnalysisSession` shall maintain explicit dirty-state tracking and document path binding:

```cpp
namespace streamview::app {

class AnalysisSession {
public:
    [[nodiscard]] bool isDirty() const noexcept;
    void markDirty() noexcept;
    void clearDirty() noexcept;

    [[nodiscard]] const std::optional<QString>& sessionFilePath() const noexcept;
    void setSessionFilePath(QString path);

    [[nodiscard]] bool save(QString* errorMessage = nullptr);
    [[nodiscard]] bool saveAs(const QString& path, QString* errorMessage = nullptr);
};

} // namespace streamview::app
```

#### Mutation Points & Dirty Semantics
1. **Clean State (`isDirty() == false`)**:
   - Immediately upon opening a fresh media file via `AnalysisSession::openFile()`.
   - Immediately upon successfully restoring a session via `AnalysisSession::restoreSession()`.
   - Immediately upon a successful `save()` or `saveAs()`.
2. **Dirty State (`isDirty() == true`)**:
   - Adding, modifying, or removing a user bookmark (`addBookmark`, `removeBookmark`).
   - Adding, modifying, or removing a user annotation (`addAnnotation`, `removeAnnotation`).
   - Applying a format manual override on an already loaded file (re-analyzing under a different rule).
3. **Non-Mutating State (Does NOT mark dirty)**:
   - Expanding or collapsing tree nodes in `AnalysisTreeView` (transient inspection).
   - Scrolling or paging through `RawDataView` or `TimelineTableView` (transient inspection).
   - Selecting a node or source span (transient selection).
   - Entering or returning from a child sample or structural entry (transient navigation).

#### UI Guardrail Protocol (`maybeSave()`)
In `MainWindow`, any action that would discard the current session (`openFile()`, `openSession()`, `closeEvent()`, or `QApplication::quit()`):
1. Checks `session_ && session_->isDirty()`.
2. If clean, proceeds immediately.
3. If dirty, displays a modal confirmation message box:
   - Text: *"The current session has unsaved changes. Do you want to save your changes before proceeding?"*
   - Buttons: `[Save]` (default), `[Discard]`, `[Cancel]`.
4. **Action Handling**:
   - `[Save]`: Calls `saveSession()`. If successful, proceeds with the requested close/open action; if cancelled or failed, aborts the action.
   - `[Discard]`: Discards unsaved changes and proceeds with the requested action.
   - `[Cancel]`: Aborts the requested action immediately; the current session remains active and unchanged.

---

### 4. Format Manual Override Architecture (Closing P2-17, P2-19, P2-20)

#### 1. Interactive Ambiguity Resolution (P2-17)
In `MainWindow`, when `session_->formatSelection().ambiguous()` is true:
- The ambiguity banner surfaced in Task P5j-5 displays an interactive **"Resolve Ambiguity..."** button alongside the warning.
- Clicking this button opens the Format Manual Override dialog pre-populated with the conflicting candidate formats (e.g. `MP4 (ISOBMFF)` vs `H.264 (Annex B)`).

#### 2. Explicit Menu Action
A top-level menu action `Analysis > Override Format...` (`actionOverrideFormat`) is enabled whenever a session is active.

#### 3. Format Override Execution Contract
When a user selects a target rule entry point:
1. `AnalysisSession::overrideFormat(const rules::RuleEntryPointIdentity& targetRule)`:
   - Requests cancellation on any running background analyzer tasks.
   - Clears the navigation stack and resets active selection.
   - Queries `RulePackageCatalog` for the selected `targetRule`.
   - Re-constructs the appropriate analyzer (e.g. `H264AnnexBAnalyzer`, `AacAdtsAnalyzer`, or `Mp4IsobmffAnalyzer`).
   - Restarts analysis from byte 0.
   - Re-initializes `AnalysisTreeModel` and `TimelineTableModel`.
   - Marks the session as **dirty** (`markDirty()`).
2. **P2-19 Resolution**:
   - Heuristic stream priorities in `format_selection.cpp` (such as `aacStrong && !h264Strong`) serve only as auto-detection fallbacks.
   - Manual override provides an unambiguous, definitive user choice that bypasses all heuristic arbitration gates.
3. **P2-20 Resolution**:
   - Tests shall clearly separate:
     a) End-to-end auto-detection arbitration tests (`openFile()`), verifying correct detection without pinning.
     b) Manual rule override / pinned-rule tests (`openWithExplicitRule()`), verifying that explicit rule selection correctly forces the target analyzer even when byte patterns conflict.

---

### 5. Rule Version Management Architecture

A dedicated dialog `RuleManagerDialog` (invoked via `Tools > Manage Rules...`) provides management of format rules:
1. **Catalog Inspection**:
   - Lists all packages discovered by `RulePackageStore` / registered in `RulePackageCatalog`.
   - Displays for each package: `packageId`, `packageVersion`, `description`, `contentSha256` (abbreviated with tooltip), and list of available `entryPoints`.
   - Highlights package origin: `[Bundled]` (official immutable assets) vs `[Installed]` (user storage).
2. **Package Installation**:
   - Provides an **"Install Package (.svrule)..."** action.
   - File picker allows selecting `.svrule` ZIP archives.
   - Invokes `RulePackageStore::install()`, verifying all security checks (path traversal rejection, zip bomb prevention, SHA-256 integrity, manifest schema validation per ADR-0015 and ADR-0016).
   - On success, registers the new package in `RulePackageCatalog` and refreshes the view.
3. **Bundled Protection**:
   - Bundled official rules cannot be uninstalled or overwritten.

---

### 6. Phase 6 Work Breakdown Structure (P6a – P6i)

To ensure strict compliance with project discipline (independent SOP closed loops, no mixing of capabilities with consumers, and mandatory review gates), Phase 6 is partitioned into the following sequential task slices:

- **Task P6a**（Specification & Lifecycle Architecture — *Current Task*）：
  - Dual-language ADR-0109 defining session lifecycle, dirty tracking, format manual override, rule manager, and schema evolution policy (Markdown-only).
  - Milestone review gate per discipline rule 5.
- **Task P6b**（Session Lifecycle & Dirty State Core Slice）：
  - `AnalysisSession` / `SessionDocument` dirty state tracking (`isDirty`, `markDirty`, `clearDirty`), file path binding, `save()` and `saveAs()` core APIs.
  - Unit tests in `analysis_session_test` and `session_document_test` validating dirty transitions and atomic saving.
- **Task P6c**（UI Actions & Unsaved Changes Guardrail Slice）：
  - `MainWindow` File > Save, Save As, and Open Session action integration.
  - `closeEvent` and `maybeSave()` confirmation dialog protocol (Save / Discard / Cancel).
  - UI tests in `main_window_test` simulating dirty session close, discard, and cancel branches.
- **Task P6d**（Format Manual Override & Ambiguity Resolution Slice）：
  - `AnalysisSession::overrideFormat` API and `FormatOverrideDialog`.
  - Ambiguity banner "Resolve Ambiguity..." integration (closing P2-17, P2-19, P2-20).
  - End-to-end tests verifying format override and re-analysis.
- **Task P6e**（Rule Version Management Slice）：
  - `RuleManagerDialog` UI listing bundled and installed packages.
  - External `.svrule` installation integration via `RulePackageStore`.
  - Dialog unit and integration tests.
- **Task P6f**（Progress, Cancellation & Diagnostics Summary Dock Slice）：
  - Analysis progress bar indicator and interactive cancellation button.
  - `DiagnosticsSummaryDock` displaying all warnings/errors across the file with bidirectional selection into the analysis tree and raw view.
- **Task P6g**（Desktop Experience, Theme & Bilingual Localization Slice）：
  - Light and Dark application theme toggle.
  - Dynamic runtime language switching (English / Simplified Chinese) using `QTranslator` and compiled `.qm` resources.
  - UI tests for theme and language transitions.
- **Task P6h**（End-to-End Regression & Persistence Verification Slice）：
  - Full session lifecycle regression: save, modify, reload, format override persistence, and 100 GB virtual sparse source session restoration.
- **Task P6i**（Phase 6 Review & Milestone Closure Gate）：
  - Dual-language documentation synchronization, checklist 100% verification, and milestone review gate.

---

## Consequences

### Positive
- **Guaranteed Compatibility**: Retaining Schema Version 1 eliminates schema version churn, migration logic, and risks of breaking existing session files.
- **Data Protection**: Users are protected from accidental data loss upon window close, file open, or quit.
- **Deterministic Ambiguity Resolution**: Format detection ambiguities (P2-17/P2-19) receive a clean, user-guided resolution path.
- **Architectural Clarity**: The distinction between auto-detection arbitration and pinned rule override is formalized in both code and tests (P2-20).
- **Clean Decoupling**: Transient UI navigation state remains separated from persistent coordinate documents.

### Negative / Trade-offs
- Transient navigation stack (e.g. being 3 levels deep inside a nested sample) is not restored upon reloading a session; the session restores at the container level with the selected byte offset highlighted. (This is an intentional design choice to guarantee stability and prevent brittle deserialization).

---

## Alternatives Rejected

1. **Increment Schema to Version 2 to Add `"formatOverride": true`**:
   - *Rejected*: The `rule` object already records the exact `RuleEntryPointIdentity` used to analyze the file. Session restoration directly instantiates the recorded rule from the catalog without running auto-detection. An explicit boolean flag would be redundant.
2. **Serialize Navigation Stack into `SessionDocument`**:
   - *Rejected*: In accordance with ADR-0103 §6, ADR-0105 §6, and §4.3, child sample execution contexts are dynamic inspection overlays, not durable document anchors. Serializing ephemeral execution trees would tightly couple `.svsession` files to runtime implementation details.
3. **Allow Unknown Keys in Schema Version 1 for "Forward Compatibility"**:
   - *Rejected*: ADR-0033's closed-schema design (`hasExactKeys`) is essential for security (rejecting malformed files), catching typos, and ensuring strict determinism.
4. **Mark Session Dirty on Tree Expansion or Raw Data Scrolling**:
   - *Rejected*: Paging and expanding are exploratory inspection activities, not user-authored modifications. Marking dirty on every scroll would result in false-positive save prompts.
