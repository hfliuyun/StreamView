# ADR-0115: WAV/RIFF Container Support — Scanner, Detector, Official Rule Package, and Activation

## Status

Accepted

## Date

2026-10-14

## Context

Phase 8 of the StreamView implementation plan introduces formal support for
WAV audio files: the Resource Interchange File Format (RIFF) container with
the WAVE form type. WAV is the first format in the project whose framing is
entirely little-endian and whose chunk chain carries runtime-declared payload
sizes, and it is the first format whose file opens with a dedicated
non-chunk form header (12 bytes) ahead of the chunk sequence.

Three architectural gates were probed against the current language and
runtime before this decision, per the scratch-probe discipline:

1. **The lazy-to-little-endian gate** (`docs/format-language/README.md:618`).
   `scratch/wav_probe2_chain.svfmt` verified that a big-endian
   `bits<32>` field placed after a runtime-sized `@lazy` region is accepted,
   while `scratch/wav_probe3_le_after_lazy.svfmt` reproduced the documented
   rejection for the little-endian field the chain actually needs:

   ```text
   scratch/wav_probe3_le_after_lazy.svfmt:14:5: error: Little-endian fields must begin at a byte boundary within the structure
   ```

   A RIFF chunk chain therefore cannot self-advance inside the DSL: the next
   chunk's little-endian size field is unreachable after a runtime-sized
   payload. A host-side scanner in `src/rules/` is required, following the
   MP4 precedent (`Mp4BoxScanner`, commit `a228304`).

2. **Conditional and assertion boundaries.** The first dispatch probe
   (`scratch/wav_probe4_header_dispatch.svfmt`) exposed four additional
   language limits that shaped the grammar below:

   ```text
   wav_probe4_header_dispatch.svfmt:30:33: error: Conditions require a field or context_value equality
   wav_probe4_header_dispatch.svfmt:39:40: error: Invalid character in DSL source          (ternary `?:` unsupported)
   wav_probe4_header_dispatch.svfmt:19:9: error: Assertions must be top-level or repeat-local items
   wav_probe4_header_dispatch.svfmt:51:21: error: Only h264_start_code, adts_frame, and mp4_box are supported
   ```

3. **Switch-arm namespace.** `scratch/wav_probe5_final_shape.svfmt` first
   failed with `Duplicate field name` for `cb_size` declared in two switch
   arms, confirming that switch arms share one structure-level namespace. The
   corrected probe passes every static check; its sole remaining error is the
   scanner allowlist gate, which the W2 capability slice closes:

   ```text
   wav_probe5_final_shape.svfmt:68:21: error: Only h264_start_code, adts_frame, and mp4_box are supported
   ```

`AnalysisSession` (`src/app/analysis_session.cpp:353-359`) and
`svtool analyze` (`tools/svtool/main.cpp:137-149`) currently arbitrate three
formats and fall back to H.264 otherwise; commit `11330e5` already surfaces
the undecided case explicitly, so a not-confidently-recognized file is no
longer silently mislabeled.

## Decision

### 1. Format classification (contract Step 1)

- **Container**, chunk-delimited; RIFF forms carry codec payloads as
  transport. WAV here means the RIFF/WAVE file family, decided from bytes,
  never from a filename extension.
- **Little-endian throughout** except raw byte identifiers (FourCCs).
- **Top-level format-specific entry point**, not a cross-layer payload view.
- Chunk framing: `chunk_id` (4 raw bytes) + `chunk_size` (unsigned 32-bit
  little-endian byte count) + payload + one pad byte when the payload size is
  odd. The form is bounded by the 12-byte header (`'RIFF'` + form size +
  `'WAVE'`); the form end is `8 + riff_size`.

### 2. Scanner capability slice (Task W2)

- Add `RiffChunkScanner` (`src/rules/include/streamview/rules/riff_chunk_scanner.h`,
  `src/rules/riff_chunk_scanner.cpp`): walks the chunk chain from offset 12,
  stepping `P' = P + 8 + size + (size & 1)`, clamping payloads at source end
  with truncated-source diagnostics, and stopping (without resynchronization)
  when the next header cannot be framed.
- The scanner emits the 12-byte RIFF form header as the **first record**,
  spanning `[0, 12)`. This resolves the missing "file header + chunk
  sequence" precedent inside the existing DSL surface: the sequence element
  struct dispatches on the first FourCC.
- Extend `DslScannerKind` with `RiffChunk` and admit `scan(riff_chunk)` in
  the parser and compiler allowlists (`src/rules/dsl.cpp:3651-3655`,
  `src/rules/dsl_ir.cpp:3929-3938`), with language-reference updates in both
  English and Chinese.
- Progressive, cancellable, and resumable in place, mirroring
  `Mp4BoxScanner` (ADR-0027 semantics via `@index(progressive)`).

### 3. Detector (Task W2)

`detectRiffWaveCandidate` (`src/rules/riff_wave_detector.cpp`) probes the
initial 64 KiB page:

- **Strong**: `'RIFF'` at offset 0, `'WAVE'` at offset 8, and at least two
  gapless well-framed chunks anchored at offset 12; a trailing truncated
  chunk does not break Strong (truncation immunity, mirroring MP4).
- **Probable**: magics verified with exactly one well-framed chunk.
- **Weak**: `'RIFF'` magic with a broken or unverifiable chain.

**False-positive boundaries.** Arbitrary bytes that happen to open with
`'RIFF'` but do not tile remain Weak/Probable and lose arbitration, leaving
detection undecided (surfaced by the undecided-format banner from commit
`11330e5`). The byte string `'RIFF'` inside an MP4 `mdat` is subsumed by the
MP4 tiling through the containment predicate. If both MP4 and WAV tilings
reach Strong (pathological, since their anchor bytes are disjoint), MP4 keeps
its documented precedence. `RIFX` (`0x52494658`), `RF64` (`0x52463634`), and
BW64 are **not detected** in v0.1 — see postponements.

### 4. Official rule package (`org.streamview.wav`, Task W3)

Manifest `src/rules/official/org.streamview.wav/rule.toml`, initial version
**0.1.0**, `manifest-version = 1`, `language = "0.1"`,
`engine = ">=0.1.0 <0.2.0"`; one entry point:

```toml
[[entrypoints]]
id = "wave"
format = "audio.wav"
source = "src/wav_riff.svfmt"
profiles = ["wave"]
depth = "chunks"
detector = "riff-wave"
```

Grammar shape (probe-verified): the sequence element struct reads the first
FourCC and dispatches:

- `fourcc == 0x52494646` (`'RIFF'`): form header record — `riff_size`
  (LE32) and `wave_id` (`'WAVE'`, `0x57415645`).
- `fourcc == 0x666D7420` (`'fmt '`): the 16 common WAVEFORMATEX bytes
  (`audio_format`, `num_channels`, `sample_rate`, `byte_rate`,
  `block_align`, `bits_per_sample`, all little-endian), then
  `switch (chunk_size)` with `case 18` (`fmt_cb_size`), `case 40`
  (`extensible_cb_size`, `valid_bits_per_sample`, `channel_mask`,
  `subformat_guid_tail[12]`), and `default` registering
  `@lazy(chunk_size - 16) bytes fmt_tail`.
- `fourcc == 0x66616374` (`'fact'`): `sample_length` (LE32) when
  `chunk_size == 4`, otherwise an opaque lazy payload.
- everything else (`data` `0x64617461`, `LIST` `0x4C495354`, `JUN K`
  `0x4A554E4B`, unknown): `@lazy(chunk_size) bytes chunk_payload`.

Value-domain rules:

- `audio_format` is a **plain 16-bit field without `@enum`**: the tag space
  is open-ended, and a closed enum would fatally reject valid files. Common
  tags (1 PCM, 3 IEEE float, 6 A-law, 7 μ-law, 17 IMA ADPCM, 0xFFFE
  extensible) are documented in the field description, following the
  ADR-0040/ADR-0059 non-fatal dichotomy.
- `audio_format == 0xFFFE` is validated by shape (`chunk_size == 40` with
  `cb_size == 22` displayed), not by tag-gated branches.
- PCM samples are **never decoded**: `data` remains a lazy opaque region,
  consistent with the read-only bit-level scope.

### 5. Detection and activation (Task W3)

- `DetectedFormat::RiffWave` joins the arbitration in
  `src/rules/format_selection.cpp` as a second container tiling: WAV coverage
  ends at the furthest well-framed chunk, and the existing
  elementary-stream containment predicates extend to it. Its signature grows
  to four detection results; all call sites (`AnalysisSession`, `svtool`,
  `format_selection_test`) are updated together.
- `SessionAnalyzerVariant` gains `WavRiffAnalyzer`
  (`src/rules/wav_riff_analyzer.cpp`); `setupResolvedAnalyzer` dispatches on
  package id `org.streamview.wav`, and the undecided path keeps its
  commit-`11330e5` behavior unchanged.
- `svtool analyze` gains the WAV dispatch branch. The Qt app needs no new
  format-specific strings: the override dialog reads the catalog, and the
  timeline reports "Container tracks unavailable" (WAV has no sample table;
  the `fact` sample count is a later slice).
- Package registration in `src/rules/CMakeLists.txt` via `qt_add_resources`,
  with bilingual package docs under
  `src/rules/official/org.streamview.wav/docs/`.

### 6. Malformed, truncated, unsupported, cancelled, and limit semantics

- **Truncated header** (< 12 bytes) or truncated trailing chunk: clamped
  record with a truncated-source diagnostic; partial results preserved.
- **Malformed sizes**: `chunk_size` values that overflow the form or the
  source are clamped at the bound; `riff_size < 4` is rejected by the
  scanner (moved out of the rule because nested assertions are rejected by
  the language, per the probe); bytes beyond the form end are unclaimed
  trailing data with one non-fatal diagnostic.
- **Unsupported**: `RIFX`, `RF64`, and `ds64` are outside v0.1 and simply do
  not produce a WAV candidate (the undecided notice covers them). A
  dedicated unsupported diagnostic for RIFF-family magics would require
  detection-layer taxonomy changes and is postponed with them.
- **Recovery**: length-chain stepping gives no resynchronization point;
  after an unframable chunk the scanner stops. A follow-on-valid-unit
  fixture therefore does not apply, and this is recorded as a deliberate
  boundary.
- **Limits and cancellation**: scanner batches mirror the MP4 analyzer's
  bounded-batch publication, node and instruction budgets are the existing
  VM/runtime budgets, and cancellation leaves a resumable progressive index.

## Phased Implementation Sequence

- **Task W1 (current)**: probe report and bilingual ADR-0115 (this commit,
  Markdown-only; hosted CI skipped per ADR-0019).
- **Task W2**: `RiffChunkScanner`, `RiffWaveDetector`, `DslScannerKind::RiffChunk`
  lowering and allowlist, bilingual language-reference updates, scanner/
  detector/dsl capability tests (`feat(rules)`).
- **Task W3**: official package `org.streamview.wav` v0.1.0, bilingual package
  docs, resource registration, `WavRiffAnalyzer`, four-way arbitration,
  session/svtool activation, deterministic fixture generator and matrix,
  analyzer/application tests, fuzz driver (`feat(wav)`).
- **Task W4**: dev/ci/sanitize full matrix, hosted three-platform CI,
  `git diff --check`, and the implementation-plan record (`docs: record`).

### Fixture and test matrix (contract Step 5)

All fixtures are emitted by a checked-in generator
(`tests/fixtures/generate_wav_fixtures.py`):

1. `wav_w2_minimal_pcm.wav` — 8-bit mono 8 kHz, 16-byte `fmt `, 4-byte `data`.
2. `wav_w2_pcm16_stereo.wav` — 16-bit stereo 44.1 kHz with `fact` and `LIST`
   and an odd-size unknown chunk exercising the pad byte.
3. `wav_w2_extensible.wav` — `WAVE_FORMAT_EXTENSIBLE`, `fmt ` size 40.
4. `wav_w2_truncated_header.wav` — 8 bytes (`'RIFF'` + size, no `'WAVE'`).
5. `wav_w2_truncated_chunk.wav` — `data` size extending past EOF.
6. `wav_w2_malformed_sizes.wav` — `riff_size < 4`; a `0xFFFFFFFF` chunk size.
7. `wav_w2_trailing_bytes.wav` — valid form followed by unclaimed bytes.
8. `wav_w2_riff_in_mp4.mp4` — a valid MP4 whose `mdat` contains `'RIFF'`
   bytes (detector false-positive containment).

Tests: `riff_chunk_scanner_tests`, `riff_wave_detector_tests`,
`dsl_test`/`dsl_ir_test` allowlist cases, `wav_riff_analyzer_tests`
(bit-accurate field acceptance per fixture, truncated/malformed diagnostics),
`format_selection_tests` four-way arbitration, `analysis_session_tests`
end-to-end open of fixtures 1-3, and a `riff_chunk_scanner` fuzz driver
following the ADR-0112 pattern. A large/sparse-source case reuses the
in-test sparse-source approach rather than a repository fixture.

## Consequences

### Positive

- WAV files open with full chunk-tree and `fmt ` field semantics at exact
  bit offsets; unrecognized files keep the honest undecided banner.
- The scanner follows the accepted MP4 pattern; no core or DSL-semantics
  change beyond the scanner allowlist.

### Negative

- The fmt slice is size-exact (16/18/40): fmt chunks with non-canonical
  sizes degrade to an opaque tail. Extensible chunks smaller or larger than
  40 bytes are not structurally decoded in v0.1.
- Compressed WAVE payloads remain opaque; RF64/RIFX/BW64 are undetected.

## References

- Microsoft & IBM, "Multimedia Programming Interface and Data Specifications
  1.0" (August 1991) — RIFF chunk layout, WAVE form definition, `fmt `/`data`/
  `fact` chunks
- [MS-RIFF] — Microsoft Resource Interchange File Format (RIFF) File Format
- Microsoft Learn, WAVEFORMATEX / WAVEFORMATEXTENSIBLE structure reference
- RFC 2361 — WAVE and AVI Codec Registries (informational, tag meanings)
- ADR-0010 (format language), ADR-0011 (dual coordinates), ADR-0025 (excluded
  spans), ADR-0027 (progressive resume), ADR-0040/0059 (non-fatal dichotomy),
  ADR-0092 (ADTS scanner/detector precedent), ADR-0099 (MP4 package and
  activation precedent), ADR-0112 (scanner fuzzing), ADR-0114 (format
  extension delivery contract)
