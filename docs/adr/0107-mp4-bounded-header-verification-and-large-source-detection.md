# ADR-0107: MP4 Bounded Header Verification and Large Source Detection

- **Status**: Proposed
- **Date**: 2026-08-30
- **Authors**: StreamView Contributors

---

## Context

StreamView performs format detection over a bounded prefix of a local analysis
source (by default 64 KiB, `mp4DetectionProbeSizeBytes()`). In
`src/rules/mp4_box_detector.cpp`, `detectMp4Candidate` scans contiguous boxes
starting from offset 0, recording each validated box into `evidence` and assigning
a confidence:
- 3 or more boxes: `Strong`
- 2 boxes: `Probable`
- 1 box: `Weak`

In ADR-0106, format arbitration in `src/rules/format_selection.cpp` established
that an MP4 candidate must reach `Strong` confidence to claim the stream against
pattern detectors (H.264 Annex B or AAC ADTS).

### The Defect: Large Boxes Discarded at Window Boundary

In ISO/IEC 14496-12 media files, media payload boxes (`mdat`) are routinely
enormous (megabytes to gigabytes). When scanning a typical faststart movie
layout (`ftyp` + `moov` + large `mdat`):
1. `ftyp` (typically 24–32 bytes) is fully verified within the 64 KiB window.
2. `moov` (typically 1–10 KiB) is fully verified within the 64 KiB window.
3. `mdat` begins at an offset inside the 64 KiB window, and its 8-byte (or
   16-byte largesize) header is fully present within the window. However, its
   declared length (`boxSize`) extends far past the 64 KiB probe limit.

In the prior implementation (`mp4_box_detector.cpp:108-111`):

```cpp
if (boxSize > remainingInInspection) {
    // Box extends beyond inspection window or is truncated in source
    break;
}
```

The scanner discarded `mdat` entirely upon encountering `boxSize > remainingInInspection`.
Consequently:
- The detector recorded only 2 boxes (`ftyp` and `moov`), yielding only
  `Probable` confidence instead of `Strong`.
- In `format_selection.cpp`, because `mp4Strong` requires `confidence == Strong`,
  container arbitration could not claim the stream. Because an AVC-in-MP4 `mdat`
  contains valid H.264 NAL headers, `detectH264AnnexBCandidate` reached `Strong`.
  Since `mp4Strong` was false, the source fell back to `H264AnnexB`.
- In non-faststart files (`ftyp` + large `mdat` + trailing `moov`), discarding
  `mdat` left only 1 box (`ftyp`) -> `Weak`.
- In files starting with a 64-bit largesize `mdat` (`size == 1`), discarding
  `mdat` left 0 boxes (`nullopt`), and offset 0 was misparsed as an anchored
  H.264 NAL header (`00 00 00 01 6D ...` -> type 13).

### Constraints

1. **Do not relax box count thresholds**: Review ruling (P5j-4d ruling 3)
   explicitly forbade simply lowering the `Strong` threshold to 2 boxes. A
   two-box tiling is insufficiently specific and would create false positives on
   short arbitrary data.
2. **Preserve coverage end invariant**: ADR-0106 relies on `boxSpan` to
   compute `mp4CoverageEndBytes`. The uninspected body of a window-exceeding box
   must **never** be included in `boxSpan` or `coverageEnd`, otherwise
   uninspected payload bytes would be falsely claimed as structurally verified.

## Decision

### 1. Bounded Header Verification for Window-Exceeding Boxes

When `detectMp4Candidate` encounters a box whose declared `boxSize` exceeds
`remainingInInspection`:
1. Check if the box is truncated in the source file:
   ```cpp
   const quint64 remainingInSource = sourceSizeBytes - offset;
   if (boxSize > remainingInSource) {
       break; // Truncated in source
   }
   ```
   If the declared box size exceeds the actual bytes remaining in the source,
   the file is truncated and the box is invalid.
2. Check bit coordinate overflow:
   ```cpp
   if (!checkBitCoordinateOverflow(offset, boxSize)) {
       break;
   }
   ```
3. Verify that the box header is fully contained within the inspected window
   (`remainingInInspection >= headerBytes`, where `headerBytes` is 16 for
   `size == 1` and 8 for `size >= 8`). This check is already satisfied prior to
   reading `size` and `largesize`.
4. Record the box into `evidence` with a **header-only source span**:
   ```cpp
   const auto span = core::SourceSpan::create(
       core::SourceBitAddress(offset * 8U), headerBytes * 8U);
   if (!span.has_value()) {
       break;
   }

   Mp4DetectionEvidence ev;
   ev.boxSpan = span;
   ev.boxOffset = offset;
   ev.declaredBoxSize = declaredSize;
   evidence.push_back(ev);
   break;
   ```
5. Terminate scanning (`break;`), because the next box begins at `offset + boxSize`,
   which lies beyond the probe window.

### 2. Header-Only Span Protects Coverage End

By setting `ev.boxSpan` to span only the verified `headerBytes` (8 or 16 bytes):
- All evidence spans end at or before `result.inspectedByteCount`.
- `mp4CoverageEndBytes` extends only through the verified header of the
  window-exceeding box (e.g. `ftyp` + `moov` + 8 bytes of `mdat`).
- Uninspected payload bytes inside `mdat` remain beyond `coverageEnd`. Any
  pattern evidence (e.g. H.264 start codes in `mdat`) evaluates to
  `!contained`. But because the first start code is not at offset 0
  (`!h264Anchored`), `h264Independent` remains false.
- Faststart movies (`ftyp` + `moov` + `mdat`) have 3 boxes in `evidence` and
  reach `Strong`. Container arbitration selects `Mp4Isobmff` with reason
  `Mp4SubsumesPatternEvidence`.

## Consequences

### Positive

- Real-world large MP4 files (e.g. `ftyp` + `moov` + multi-gigabyte `mdat`)
  achieve `Strong` confidence and are correctly recognized as `Mp4Isobmff`.
- The coverage end invariant of ADR-0106 is preserved: uninspected bytes are
  never claimed as covered.
- 64-bit largesize box headers (`size == 1`) within the probe window are
  recorded into `evidence`.
- Truncated boxes (where `boxSize > sourceSizeBytes - offset`) continue to be
  rejected.

### Negative and Deferred

- Non-faststart files (`ftyp` + large `mdat` + trailing `moov`) record only 2
  boxes (`ftyp` and `mdat` header) in the 64 KiB probe window, reaching `Probable`.
  Recognizing non-faststart containers when pattern evidence is present will
  require two-ended probing (reading the tail of the source for `moov`), which
  is deferred to a dedicated container probing slice.

## Verification

1. `tests/rules/mp4_box_detector_test.cpp`:
   - `acceptsWindowExceedingBoxWhenHeaderIsVerifiable`: normal box (32768 + 32732 + 1000 at 65500) reaches `Strong` with 3 boxes; 3rd box span covers exactly 8 header bytes ending at 65508.
   - `acceptsWindowExceedingLargeBoxWhenHeaderIsVerifiable`: large box (32768 + 32732 + large box 1000 at 65500) reaches `Strong` with 3 boxes; 3rd box span covers exactly 16 header bytes ending at 65516.
   - `evidenceSpanExceedingProbeWindowIsRejected`: 4th box header at 60000 (declaring 10000) is included as 4th evidence entry with span ending at 60008 <= 65536.
   - Synthetic real-scale fixture (`ftyp` 32 + `moov` 2000 + 500 MB `mdat`) achieves `Strong` confidence with `evidence.size() == 3`.
   - Truncated window-exceeding box (`boxSize > sourceSizeBytes - offset`) is rejected from `evidence`.
2. `tests/rules/format_selection_test.cpp`:
   - Synthetic real-scale MP4 (`ftyp` + `moov` + 500 MB `mdat` with AVC NAL start codes in `mdat`) selects `Mp4Isobmff` with reason `Mp4SubsumesPatternEvidence`.
3. All existing test suites pass across `dev`, `ci`, and `sanitize` presets.

## References

- [ADR-0099: MP4 Official Rule Package and Activation](0099-mp4-official-rule-package-and-activation.md)
- [ADR-0106: Format Detection Arbitration and Evidence Asymmetry](0106-format-detection-arbitration-and-evidence-asymmetry.md)
- ISO/IEC 14496-12, Section 4.2 (Box structure)
