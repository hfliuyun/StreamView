#include <streamview/rules/format_selection.h>

#include <algorithm>

namespace streamview::rules {

namespace {

/// End of the byte range the box tiling actually verified. Every recorded box is
/// contiguous from offset 0, so the furthest box end is the coverage end.
[[nodiscard]] quint64 mp4CoverageEndBytes(const Mp4DetectionResult& mp4) noexcept {
    if (!mp4.candidate.has_value()) {
        return 0;
    }
    quint64 coverageEnd = 0;
    for (const auto& evidence : mp4.candidate->evidence) {
        if (!evidence.boxSpan.has_value()) {
            continue;
        }
        const quint64 boxEnd = evidence.boxSpan->endExclusive().absoluteBitOffset() / 8U;
        coverageEnd = std::max(coverageEnd, std::min(boxEnd, mp4.inspectedByteCount));
    }
    return coverageEnd;
}

/// True when every start code sits inside the range the box tiling explains, so
/// the pattern carries no evidence the container has not already accounted for.
/// An empty candidate is vacuously contained and cannot outrank a tiling.
[[nodiscard]] bool h264EvidenceIsContained(const H264AnnexBDetectionResult& h264,
                                           quint64 coverageEndBytes) noexcept {
    if (!h264.candidate.has_value()) {
        return true;
    }
    for (const auto& evidence : h264.candidate->evidence) {
        if (!evidence.startCode.has_value()) {
            continue;
        }
        if (evidence.startCode->start().absoluteBitOffset() / 8U >= coverageEndBytes) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool aacEvidenceIsContained(const AacAdtsDetectionResult& aac,
                                          quint64 coverageEndBytes) noexcept {
    if (!aac.candidate.has_value()) {
        return true;
    }
    for (const auto& evidence : aac.candidate->evidence) {
        if (!evidence.syncword.has_value()) {
            continue;
        }
        if (evidence.syncword->start().absoluteBitOffset() / 8U >= coverageEndBytes) {
            return false;
        }
    }
    return true;
}

/// An Annex B byte stream opens with a start code prefix, optionally preceded by
/// zero bytes (ITU-T H.264 Annex B). A start code that first appears deep inside
/// a source is a byte coincidence, not a stream start. The allowance of up to
/// three leading zero bytes exists solely to accommodate compliant Annex B
/// streams opening with leading zero bytes; container exclusion relies on the
/// containment predicate rather than this window.
constexpr quint64 maximumAnnexBLeadingZeroBytes = 3U;

[[nodiscard]] bool h264IsAnchoredAtSourceStart(
    const H264AnnexBDetectionResult& h264) noexcept {
    if (!h264.candidate.has_value()) {
        return false;
    }
    for (const auto& evidence : h264.candidate->evidence) {
        if (!evidence.startCode.has_value()) {
            continue;
        }
        return evidence.startCode->start().absoluteBitOffset() / 8U <=
               maximumAnnexBLeadingZeroBytes;
    }
    return false;
}

[[nodiscard]] bool aacIsAnchoredAtSourceStart(
    const AacAdtsDetectionResult& aac) noexcept {
    if (!aac.candidate.has_value()) {
        return false;
    }
    for (const auto& evidence : aac.candidate->evidence) {
        if (!evidence.syncword.has_value()) {
            continue;
        }
        return evidence.syncword->start().absoluteBitOffset() == 0U;
    }
    return false;
}

} // namespace

FormatSelection
selectFormatFromDetection(const Mp4DetectionResult& mp4,
                          const AacAdtsDetectionResult& aac,
                          const H264AnnexBDetectionResult& h264) noexcept {
    const bool mp4Strong = mp4.candidate.has_value() &&
                           mp4.candidate->confidence == Mp4DetectionConfidence::Strong;
    const bool aacStrong = aac.candidate.has_value() &&
                           aac.candidate->confidence == AacAdtsDetectionConfidence::Strong;
    const bool h264Strong = h264.candidate.has_value() &&
                            h264.candidate->confidence ==
                                H264AnnexBDetectionConfidence::Strong;

    // Containment is only meaningful against a tiling strong enough to claim the
    // bytes. A Weak tiling is often just a payload byte pattern read as a box
    // size, and letting it mark evidence as "already explained" would suppress a
    // genuine elementary stream.
    const quint64 coverageEnd = mp4Strong ? mp4CoverageEndBytes(mp4) : 0;
    const bool h264Contained = h264EvidenceIsContained(h264, coverageEnd);
    const bool aacContained = aacEvidenceIsContained(aac, coverageEnd);
    const bool h264Anchored = h264IsAnchoredAtSourceStart(h264);
    const bool aacAnchored = aacIsAnchoredAtSourceStart(aac);

    // Independent means anchored at the source start *and* reaching past the
    // tiling. Anchoring alone is satisfied by a box size whose low bytes read as
    // a start code; reaching past alone is satisfied by any payload byte.
    const bool h264Independent = h264Strong && h264Anchored && !h264Contained;
    const bool aacIndependent = aacStrong && aacAnchored && !aacContained;

    if (mp4Strong && !h264Independent && !aacIndependent) {
        FormatSelection selection;
        selection.format = DetectedFormat::Mp4Isobmff;
        if ((h264Strong && h264Anchored) || (aacStrong && aacAnchored)) {
            selection.reason =
                DetectedFormatReason::AmbiguousContainerVersusElementaryStream;
        } else if (h264Strong || aacStrong) {
            selection.reason = DetectedFormatReason::Mp4SubsumesPatternEvidence;
        } else {
            selection.reason = DetectedFormatReason::Mp4StructuralTiling;
        }
        return selection;
    }

    // Unchanged from the predicate this function replaced: between two pattern
    // detectors neither one subsumes the other, so H.264 keeps precedence. This
    // slice only corrects container-versus-pattern arbitration.
    if (aacStrong && !h264Strong) {
        return {DetectedFormat::AacAdts, DetectedFormatReason::AacFrameChain};
    }

    if (h264Strong) {
        return {DetectedFormat::H264AnnexB,
                h264Anchored ? DetectedFormatReason::H264AnchoredStartCodes
                             : DetectedFormatReason::H264UnanchoredStartCodes};
    }

    return {};
}

} // namespace streamview::rules
