#pragma once

#include <streamview/core/coordinates.h>

#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace streamview::rules {

enum class Mp4DetectionConfidence : quint8 {
    Weak,
    Probable,
    Strong,
};

struct Mp4DetectionEvidence final {
    std::optional<core::SourceSpan> boxSpan;
    quint64 boxOffset = 0;
    quint64 declaredBoxSize = 0;
};

struct Mp4Candidate final {
    Mp4DetectionConfidence confidence = Mp4DetectionConfidence::Weak;
    std::vector<Mp4DetectionEvidence> evidence;
};

struct Mp4DetectionResult final {
    std::optional<Mp4Candidate> candidate;
    quint64 inspectedByteCount = 0;
    bool sourceFullyInspected = false;
};

[[nodiscard]] constexpr quint64 mp4DetectionProbeSizeBytes() noexcept {
    return 64U * 1024U;
}

[[nodiscard]] Mp4DetectionResult
detectMp4Candidate(std::span<const std::byte> sourcePrefix,
                   quint64 sourceSizeBytes);

} // namespace streamview::rules
