# ADR-0106: Format Detection Arbitration and Evidence Asymmetry

- **Status**: Proposed
- **Date**: 2026-08-30
- **Authors**: StreamView Contributors

---

## Context

StreamView performs bounded format detection over the first 64 KiB of a local
analysis source and then selects the format definition used for the analysis
session. Three detectors participate: `detectMp4Candidate`,
`detectAacAdtsCandidate`, and `detectH264AnnexBCandidate`. Each returns an
optional candidate with a three-valued confidence (`Weak`, `Probable`,
`Strong`).

Two defects were identified in how those three results were arbitrated.

### Defect 1: pattern evidence held a veto over container evidence

Both consumers of detection independently implemented the same predicate:

```text
choose MP4  when mp4 == Strong && h264 != Strong && aac != Strong
choose AAC  when aac == Strong && h264 != Strong
otherwise   H.264
```

The predicate treated the three confidences as commensurate and gave an H.264
`Strong` an unconditional veto. They are not commensurate:

- An MP4 `Strong` is a **global structural invariant**. It means three or more
  boxes tile contiguously starting at offset 0, each declaring a length that
  lands exactly on the next box header.
- An H.264 or AAC `Strong` is a **local byte pattern**. The Annex B detector
  reaches `Strong` on two valid start-code-plus-header occurrences found
  anywhere in the inspected window, with no requirement that the first one sit
  at the start of the source.

An AVC-in-MP4 file necessarily carries H.264 headers inside its `mdat`, and an
`avcC` record carries SPS/PPS bytes inside `moov`. Both routinely produce two or
more valid Annex B headers. The veto therefore fired on ordinary, well-formed
input: a real AVC-in-MP4 source was analyzed as a raw H.264 Annex B elementary
stream, and the entire container structure was never presented.

Measured on repository fixtures before the fix, `mp4_p5j4_avc_multi_nal.mp4` and
`mp4_p5j4_two_tracks.mp4` both tiled as boxes from offset 0 to EOF, both reached
MP4 `Strong`, and both were nevertheless routed to the Annex B analyzer.

### Defect 2: the predicate existed twice

The identical predicate was written independently in the application session
layer and in the internal `svtool` CLI. Two copies of a detection policy cannot
be corrected once, and can silently diverge, so the two surfaces could disagree
about the same source.

### Constraint

ADR-0099 clause 24 requires that all FourCC matching and box format semantics
remain strictly inside the DSL rule, with no FourCC string literals or constants
in C++ core, runner, scanner, or detector logic. The obvious repair — checking
whether the first box type is `ftyp` — is therefore not available.

## Decision

### 1. One arbitration point, in the rules layer

`selectFormatFromDetection` in `src/rules/format_selection.{h,cpp}` is the single
decision point. It consumes the three detection results and returns a
`FormatSelection` carrying the chosen `DetectedFormat` and a
`DetectedFormatReason`. Both the analysis session and `svtool` call it; neither
re-implements any part of the policy.

### 2. Arbitration decides on source coordinates alone

The function inspects only the geometry of the evidence spans the detectors
already produced. It reads no box type, no brand, and no codec name, so ADR-0099
clause 24 continues to hold; the implementation contains zero FourCC literals.

Three derived predicates:

- **Coverage end** — the furthest verified box end of a `Strong` MP4 tiling,
  clamped to the inspected byte count. Because every recorded box is contiguous
  from offset 0, the furthest end is the extent of the range the container
  explains. Coverage is computed only for a `Strong` tiling; a `Weak` tiling is
  frequently a payload byte pattern read as a box size, and letting it claim
  bytes would suppress a genuine elementary stream.
- **Containment** — whether every start code or syncword lies below the coverage
  end. Contained evidence carries nothing the container has not already
  accounted for.
- **Anchoring** — whether the first valid start code or syncword begins at the
  source start. ITU-T H.264 Annex B permits leading zero bytes before the first
  start code prefix, so H.264 anchoring permits up to three leading zero bytes
  (`maximumAnnexBLeadingZeroBytes = 3`); ADTS does not permit leading zero
  bytes, so AAC anchoring demands offset 0 exactly (`absoluteBitOffset() == 0`).
  A start code or syncword first appearing deep inside a source is a byte
  coincidence, not a stream start.

Pattern evidence outranks a container tiling only when it is **independent** of
it: anchored at the source start *and* reaching past the coverage end. Either
condition alone is satisfiable by coincidence — anchoring by a box size whose
low bytes read as a start code, reaching past by any payload byte — so both are
required together.

### 3. Irreducible collision resolves to the container and is marked

Bytes can simultaneously open with a valid start code or syncword and tile as
boxes across the whole window. No FourCC-free rule can settle that case in both
directions. The selection resolves to the container and reports
`AmbiguousContainerVersusElementaryStream`, which is distinct from the ordinary
container reasons. The ambiguity is recorded rather than presented as a fact, so
the decision stays reviewable. (Requirement: `ambiguous()` must be surfaced on
the user-visible UI surface in Task P5j-5).

### 4. Relative order of the two pattern detectors is unchanged

Between two pattern detectors neither subsumes the other, so H.264 retains the
precedence it had over AAC. This ADR corrects container-versus-pattern
arbitration only.

## Consequences

### Positive

- A real AVC-in-MP4 source is analyzed as a container. Verified through
  `svtool analyze` on five MP4 fixtures, all of which now materialize a box
  tree.
- Genuine Annex B input is unaffected, including input with leading zero bytes
  before the first start code.
- Detection policy is corrected in one place for every surface.
- Selection carries a reason, so an ambiguous decision is distinguishable from a
  confident one.

### Negative and deferred

- **Large real-world MP4 sources reach only `Probable`.** A file shaped
  `ftyp` + `moov` + a large `mdat` records two boxes within the inspected
  window, because the `mdat` header declares a length extending past the window
  and the detector discards boxes it cannot fully verify. Such a source does not
  reach `Strong` and therefore does not benefit from this arbitration. This
  blocker is unblocked via a dedicated detector confidence slice (Task P5j-4e)
  prior to P5j-5.
- **Unrecognized input still falls back to the H.264 analyzer.** Requirements
  demand that a source not be refused when detection does not match, so the
  fallback is retained. Its diagnostics are misleading for non-H.264 input; a
  proper unknown-source path is recorded separately.
- Anchoring uses a fixed three-byte leading-zero window for H.264. This exists
  solely to accommodate compliant Annex B streams opening with leading zero
  bytes; container exclusion relies on the containment predicate rather than
  this window. A conforming stream with a longer run of leading zero bytes would
  not be treated as anchored.

## Verification

- 14 adversarial cases in `tests/rules/format_selection_test.cpp`, covering real
  fixtures, genuine Annex B, Annex B with leading zero bytes, unanchored Annex B
  with four leading zero bytes, unanchored H.264 and AAC patterns in partial
  container tilings, a container whose first box size reads as a start code,
  irreducible H.264 and AAC collisions, contained pattern evidence,
  sub-`Strong` candidates, and unstructured bytes. No case is guarded by a
  conditional assertion or skipped.
- Single-variable reverse mutation:
  - Removing `aacAnchored` causes `unanchoredAacPatternCannotOutrankPartialContainerTiling`
    to turn red (reverting to `AacAdts`).
  - Removing `h264Anchored` causes `unanchoredH264PatternCannotOutrankPartialContainerTiling`
    to turn red (reverting to `H264AnnexB`).
  - The 44 pre-existing session tests stay green — confirming the defect was
    structurally invisible to the prior suite.
- `svtool analyze` run directly on five MP4 fixtures and two Annex B inputs.

## References

- [ADR-0099: MP4 Official Rule Package and Activation](0099-mp4-official-rule-package-and-activation.md)
- [Product requirements](../product-requirements.md)
- ISO/IEC 14496-12, box structure
- ITU-T H.264 Annex B, byte stream format
