#pragma once

#include <streamview/core/coordinates.h>

#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace streamview::rules {

enum class AacAdtsDetectionConfidence : quint8 {
    Weak,
    Probable,
    Strong,
};

struct AacAdtsDetectionEvidence final {
    std::optional<core::SourceSpan> syncword;
    std::optional<core::SourceSpan> header;
    quint16 aacFrameLength = 0;
    bool crcPresent = false;
    quint8 profile = 0;
    quint8 samplingFrequencyIndex = 0;
    quint8 channelConfiguration = 0;
};

struct AacAdtsCandidate final {
    AacAdtsDetectionConfidence confidence = AacAdtsDetectionConfidence::Weak;
    std::vector<AacAdtsDetectionEvidence> evidence;
};

struct AacAdtsDetectionResult final {
    std::optional<AacAdtsCandidate> candidate;
    quint64 inspectedByteCount = 0;
    bool sourceFullyInspected = false;
};

[[nodiscard]] constexpr quint64 aacAdtsDetectionProbeSizeBytes() noexcept {
    return 64U * 1024U;
}

[[nodiscard]] AacAdtsDetectionResult
detectAacAdtsCandidate(std::span<const std::byte> sourcePrefix,
                       quint64 sourceSizeBytes);

} // namespace streamview::rules
