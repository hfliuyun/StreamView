# ADR-0108: Timeline Sample Navigation, Dock Integration, and Format Ambiguity Presentation

- **Status**: Proposed
- **Date**: 2026-09-06
- **Authors**: StreamView Contributors

---

## Context

Phase 5 delivers non-fragmented ISO BMFF MP4/MOV container inspection, metadata tree materialization, lazy `mdat` encapsulation, windowed sample table parsing, and cross-layer navigation from media samples down to elementary codec streams.

In Task P5j-4, `AnalysisSession` established the underlying sample navigation engine:
1. `tracks()`: extracts and returns indexed tracks (`AnalysisSessionTracksResult`).
2. `samplesForTrack(request)`: returns windowed `SampleDescriptor` pages (`AnalysisSessionSamplePageResult`).
3. `enterSample(trackId, sampleIndex, catalog, options)`: executes sample payload against resolved rule packages (`video.h264.nal`, `audio.aac.asc`), establishes a `SampleNavigationFrame`, and exposes the sample sub-tree.
4. `returnToParent()`: unwinds the navigation stack and provides `restoredSample` to enable UI restoration.

### Identified Requirements & Gaps for Task P5j-5

1. **Timeline & Sample Navigation Dock UI**:
   - Users need a dedicated dock widget (`timelineDock`) at the bottom of `MainWindow` presenting tracks, paginated samples, timeline coordinates (DTS, PTS, duration), sync sample / keyframe badges, and byte extents.
   - UI memory must be bounded: the sample table must be virtualized and paginated (e.g. 256 samples per page) via `AnalysisSession::samplesForTrack`, preventing unbounded node allocation on large files.
2. **Interactive Cross-Layer Sample Navigation**:
   - Double-clicking or pressing Enter on a sample row must invoke `AnalysisSession::enterSample`.
   - The active tree in `AnalysisTreeModel` must seamlessly switch to the sample access unit sub-tree.
   - Breadcrumbs must clearly reflect the container-to-sample path: e.g., `video.mp4 > Track 1 (video/mp4) > Sample #0 [Sync]`.
   - Clicking `navigationBackButton` (`returnToParent`) must restore the container tree, re-select the originating track and sample row, and scroll the table to the restored position without losing context.
3. **Bidirectional Highlighting**:
   - Selecting a sample row in the timeline table must highlight its physical `SourceSpan` in `RawDataView`.
   - Returning from a sample restores the sample payload span highlight in `RawDataView`.
4. **Format Ambiguity and Decision Presentation (ADR-0106 & Review Ruling)**:
   - `FormatSelection::ambiguous()` and `decided()` must be surfaced visibly on the `MainWindow` UI surface. If arbitration encountered competing anchored candidates (e.g., `AmbiguousContainerVersusElementaryStream`), an ambiguity warning badge/label must alert the user.
5. **Truncated Track List Presentation (P2-13)**:
   - When an MP4 file is truncated at or before its sample tables (as recorded by `core::DiagnosticCode::TruncatedSource` or `AnalysisSessionSampleStatus::TruncatedSource`), the track list UI must explicitly report: *"File is truncated; track list may be incomplete"*, rather than misleadingly stating *"No tracks"*.

---

## Decision

### 1. Format Selection Exposure on `AnalysisSession`

`AnalysisSession` shall store the `rules::FormatSelection` computed during source opening (or synthesized during session restore with explicit rules) and expose it via:

```cpp
[[nodiscard]] const rules::FormatSelection& formatSelection() const noexcept;
```

This allows UI components to inspect `ambiguous()` and `decided()` directly without duplicating arbitration logic.

### 2. `TimelineTableModel` Architecture

A dedicated `TimelineTableModel` (derived from `QAbstractTableModel`) in `src/app/` manages paginated `SampleDescriptor` records:

- **Columns**:
  - `0: Sample #` — 0-based sample ordinal (`sampleIndex`).
  - `1: Type` — `[Sync]` for keyframes (`isSyncSample == true`), `-` otherwise.
  - `2: DTS` — Decoding timestamp in track timescale units.
  - `3: PTS` — Presentation timestamp in track timescale units.
  - `4: Duration` — Sample duration in timescale units.
  - `5: Size (bytes)` — Derived from `sourceSpans` bit length ($L / 8$).
  - `6: Bit Offset` — Absolute start bit offset from `sourceSpans`.
  - `7: Description Index` — 1-based `sampleDescriptionIndex`.
- **Performance**:
  - Holds at most one page of descriptors (default page size 256).
  - Fast column lookup and tooltip formatting.

### 3. `MainWindow` Timeline Dock & Controls

`MainWindow` adds a `QDockWidget` (`timelineDock`, object name `timelineDock`) docked in `Qt::BottomDockWidgetArea`:

1. **Header Bar**:
   - Track selector combo box (`timelineTrackComboBox`): lists available tracks with ID, format, and sample count.
   - Status / Warning label (`timelineStatusLabel`): displays status messages, especially truncation warnings (P2-13).
   - Pagination controls: Previous Page button (`timelinePrevPageButton`), Next Page button (`timelineNextPageButton`), and page summary label (`timelinePageLabel`).
2. **Table View**:
   - `QTableView` (`timelineTableView`): displays rows from `TimelineTableModel`, configured with single row selection, alternating row colors, and auto-scroll.

### 4. Interactive Navigation & Synchronization Contract

1. **Row Selection**:
   - Selecting a row in `timelineTableView` updates `MainWindow`'s source selection to `sample.sourceSpans`, immediately highlighting the sample bytes in `RawDataView`.
2. **Activation**:
   - Double-clicking or pressing Enter/Return on a sample row calls `enterSample(trackId, sampleIndex, catalog_)`.
   - On success:
     - `analysisModel_->resetFromTree(session_->activeTree())` displays the sample units.
     - Breadcrumbs update to `format > Track <id> (<targetFormat>) > Sample #<index> [Sync]`.
     - Active selection focuses on the sample's child root structure node.
3. **Return to Container**:
   - Activating `navigationBackButton_` invokes `session_->returnToParent()`.
   - The returned `restoredSample` (`SampleNavigationFrame`) supplies `trackId` and `sampleIndex`.
   - `MainWindow` restores the track in `timelineTrackComboBox`, queries the page containing `sampleIndex`, selects the corresponding row in `timelineTableView`, and restores the byte highlight in `RawDataView`.

### 5. Format Ambiguity & Truncation Handling

1. **Ambiguity Presentation**:
   - A permanent status label (`formatAmbiguityLabel`) in `statusBar()` is displayed when `session_->formatSelection().ambiguous()` is true:
     *"Warning: Ambiguous format (container vs elementary stream detected)"*.
   - When not ambiguous, the label is hidden.
2. **Truncated Track List Rendering (P2-13)**:
   - When evaluating tracks, if the root tree carries `core::DiagnosticCode::TruncatedSource` or the tracks result reports `TruncatedSource`:
     - `timelineStatusLabel` displays: *"File is truncated; track list may be incomplete"*.
     - If tracks are partially available, they are still listed in the combo box alongside the visible truncation warning.
     - If zero tracks are available, the warning is prominently shown instead of an unqualified "No tracks" message.

---

## Consequences

### Positive
- Fully closes the UI loop for container sample navigation and satisfies Phase 5 item `:246` (*"从 MP4 sample 进入 H.264/AAC 规则，并可返回容器字段"*).
- Eliminates silent omissions for truncated container files (P2-13).
- Exposes format detection arbitration ambiguity to end users as required by ADR-0106.
- Virtualized pagination ensures rock-solid memory consumption for massive media files.

### Negative / Trade-offs
- Adds an additional dock widget to `MainWindow`, requiring layout management and event filtering across tree view and table view.
