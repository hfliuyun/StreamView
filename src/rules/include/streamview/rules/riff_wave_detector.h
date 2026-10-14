#pragma once

#include <streamview/core/coordinates.h>

#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace streamview::rules {

enum class RiffWaveDetectionConfidence : quint8 {
    Weak,
    Probable,
    Strong,
};

struct RiffWaveDetectionEvidence final {
    std::optional<core::SourceSpan> chunkSpan;
    quint64 chunkOffset = 0;
    quint32 chunkId = 0;
    quint64 declaredSize = 0;
    bool isFormHeader = false;
};

struct RiffWaveCandidate final {
    RiffWaveDetectionConfidence confidence = RiffWaveDetectionConfidence::Weak;
    std::vector<RiffWaveDetectionEvidence> evidence;
};

struct RiffWaveDetectionResult final {
    std::optional<RiffWaveCandidate> candidate;
    quint64 inspectedByteCount = 0;
    bool sourceFullyInspected = false;
};

[[nodiscard]] constexpr quint64 riffWaveDetectionProbeSizeBytes() noexcept {
    return 64U * 1024U;
}

/// Detects a RIFF/WAVE candidate in the source prefix. The 12-byte form
/// header anchors the candidate; each gapless well-framed chunk adds
/// evidence. Confidence is evidence-count based (header included), matching
/// the MP4 family convention recorded in ADR-0115: header alone is Weak,
/// header plus one chunk is Probable, and header plus two or more chunks is
/// Strong. Chunks truncated inside the source add no evidence; a chunk that
/// frames fully inside the source but extends past the inspection window
/// records its verified header only.
[[nodiscard]] RiffWaveDetectionResult
detectRiffWaveCandidate(std::span<const std::byte> sourcePrefix,
                        quint64 sourceSizeBytes);

} // namespace streamview::rules
