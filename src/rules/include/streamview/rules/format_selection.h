#pragma once

#include <streamview/rules/aac_adts_detector.h>
#include <streamview/rules/h264_annex_b_detector.h>
#include <streamview/rules/mp4_box_detector.h>
#include <streamview/rules/riff_wave_detector.h>

#include <QtGlobal>

namespace streamview::rules {

enum class DetectedFormat : quint8 {
    None,
    Mp4Isobmff,
    AacAdts,
    H264AnnexB,
    RiffWave,
};

/// Why a format won, so a caller can surface an ambiguity instead of presenting
/// a coincidence as a decision. `Mp4SubsumesPatternEvidence` and
/// `AmbiguousContainerVersusElementaryStream` both yield `Mp4Isobmff`, but only
/// the latter means a competing elementary stream was equally well anchored.
enum class DetectedFormatReason : quint8 {
    NoCandidate,
    Mp4StructuralTiling,
    RiffStructuralTiling,
    Mp4SubsumesPatternEvidence,
    AmbiguousContainerVersusElementaryStream,
    AacFrameChain,
    H264AnchoredStartCodes,
    H264UnanchoredStartCodes,
};

struct FormatSelection final {
    DetectedFormat format = DetectedFormat::None;
    DetectedFormatReason reason = DetectedFormatReason::NoCandidate;

    [[nodiscard]] bool decided() const noexcept { return format != DetectedFormat::None; }
    [[nodiscard]] bool ambiguous() const noexcept {
        return reason == DetectedFormatReason::AmbiguousContainerVersusElementaryStream;
    }
};

/// Arbitrates the four bounded detectors into one decision. The confidences
/// are not commensurate: an MP4 or RIFF/WAVE `Strong` is a global invariant (a
/// gapless tiling anchored at offset 0), while an H.264 or AAC `Strong` is a
/// local byte pattern that routinely occurs inside bytes a tiling already
/// accounts for. Pattern evidence therefore only outranks a container tiling
/// when it is independent of it.
///
/// Decides on source coordinates alone. No box type, brand, chunk id, or codec
/// name is inspected here, so ADR-0099 keeps FourCC semantics inside the DSL
/// rules. When both container tilings reach Strong (pathological: their anchor
/// bytes are disjoint), MP4 keeps its documented precedence.
[[nodiscard]] FormatSelection
selectFormatFromDetection(const Mp4DetectionResult& mp4,
                          const AacAdtsDetectionResult& aac,
                          const H264AnnexBDetectionResult& h264,
                          const RiffWaveDetectionResult& riff);

} // namespace streamview::rules
