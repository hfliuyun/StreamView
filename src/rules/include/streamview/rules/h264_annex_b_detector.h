#pragma once

#include <streamview/core/coordinates.h>

#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace streamview::rules {

enum class H264AnnexBDetectionConfidence : quint8 {
    Weak,
    Probable,
    Strong,
};

struct H264AnnexBDetectionEvidence final {
    std::optional<core::SourceSpan> startCode;
    std::optional<core::SourceSpan> nalUnitHeader;
    std::optional<quint8> nalUnitType;
    bool forbiddenZeroBitIsZero = false;
};

struct H264AnnexBCandidate final {
    H264AnnexBDetectionConfidence confidence = H264AnnexBDetectionConfidence::Weak;
    std::vector<H264AnnexBDetectionEvidence> evidence;
};

struct H264AnnexBDetectionResult final {
    std::optional<H264AnnexBCandidate> candidate;
    quint64 inspectedByteCount = 0;
    bool sourceFullyInspected = false;
};

[[nodiscard]] constexpr quint64 h264AnnexBDetectionProbeSizeBytes() noexcept {
    return 64U * 1024U;
}

[[nodiscard]] H264AnnexBDetectionResult
detectH264AnnexBCandidate(std::span<const std::byte> sourcePrefix,
                          quint64 sourceSizeBytes);

} // namespace streamview::rules
