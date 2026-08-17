#include <streamview/rules/mp4_box_detector.h>

#include <algorithm>
#include <limits>

namespace streamview::rules {

namespace {

[[nodiscard]] quint32 readBigEndianU32(const std::byte* bytes) noexcept {
    return (static_cast<quint32>(static_cast<quint8>(bytes[0])) << 24) |
           (static_cast<quint32>(static_cast<quint8>(bytes[1])) << 16) |
           (static_cast<quint32>(static_cast<quint8>(bytes[2])) << 8) |
           static_cast<quint32>(static_cast<quint8>(bytes[3]));
}

[[nodiscard]] quint64 readBigEndianU64(const std::byte* bytes) noexcept {
    return (static_cast<quint64>(static_cast<quint8>(bytes[0])) << 56) |
           (static_cast<quint64>(static_cast<quint8>(bytes[1])) << 48) |
           (static_cast<quint64>(static_cast<quint8>(bytes[2])) << 40) |
           (static_cast<quint64>(static_cast<quint8>(bytes[3])) << 32) |
           (static_cast<quint64>(static_cast<quint8>(bytes[4])) << 24) |
           (static_cast<quint64>(static_cast<quint8>(bytes[5])) << 16) |
           (static_cast<quint64>(static_cast<quint8>(bytes[6])) << 8) |
           static_cast<quint64>(static_cast<quint8>(bytes[7]));
}

[[nodiscard]] bool checkBitCoordinateOverflow(quint64 offsetBytes, quint64 lengthBytes) noexcept {
    constexpr quint64 maxBytes = std::numeric_limits<quint64>::max() / 8U;
    if (offsetBytes > maxBytes || lengthBytes > maxBytes) {
        return false;
    }
    return lengthBytes <= (maxBytes - offsetBytes);
}

} // namespace

Mp4DetectionResult
detectMp4Candidate(std::span<const std::byte> sourcePrefix,
                   quint64 sourceSizeBytes) {
    Mp4DetectionResult result;
    result.inspectedByteCount = std::min<quint64>(sourcePrefix.size(), sourceSizeBytes);
    result.sourceFullyInspected = (result.inspectedByteCount >= sourceSizeBytes);

    if (sourcePrefix.empty() || sourceSizeBytes == 0U) {
        return result;
    }

    quint64 offset = 0;
    std::vector<Mp4DetectionEvidence> evidence;
    const quint64 prefixSize = static_cast<quint64>(sourcePrefix.size());

    while (offset < sourceSizeBytes) {
        const quint64 remainingInSource = sourceSizeBytes - offset;
        if (offset >= prefixSize) {
            break;
        }

        const quint64 remainingInPrefix = prefixSize - offset;
        if (remainingInPrefix < 8U) {
            break;
        }

        const std::byte* headerPtr = sourcePrefix.data() + offset;
        const quint32 size = readBigEndianU32(headerPtr);

        if (size == 0U) {
            if (!checkBitCoordinateOverflow(offset, remainingInSource)) {
                break;
            }
            const auto span = core::SourceSpan::create(
                core::SourceBitAddress(offset * 8U), remainingInSource * 8U);
            if (!span.has_value()) {
                break;
            }
            Mp4DetectionEvidence ev;
            ev.boxSpan = span;
            ev.boxOffset = offset;
            ev.declaredBoxSize = 0U;
            evidence.push_back(ev);
            break;
        }

        quint64 boxSize = 0;
        quint64 declaredSize = 0;

        if (size == 1U) {
            if (remainingInPrefix < 16U) {
                break;
            }
            const quint64 largesize = readBigEndianU64(headerPtr + 8U);
            if (largesize < 16U) {
                break;
            }
            boxSize = largesize;
            declaredSize = largesize;
        } else if (size >= 8U) {
            boxSize = size;
            declaredSize = size;
        } else {
            break;
        }

        if (boxSize > remainingInSource) {
            // Truncated in source: do not add to evidence, stop chain
            break;
        }

        if (!checkBitCoordinateOverflow(offset, boxSize)) {
            break;
        }

        const auto span = core::SourceSpan::create(
            core::SourceBitAddress(offset * 8U), boxSize * 8U);
        if (!span.has_value()) {
            break;
        }

        Mp4DetectionEvidence ev;
        ev.boxSpan = span;
        ev.boxOffset = offset;
        ev.declaredBoxSize = declaredSize;
        evidence.push_back(ev);

        offset += boxSize;
    }

    if (evidence.empty()) {
        return result;
    }

    Mp4Candidate candidate;
    if (evidence.size() >= 3U) {
        candidate.confidence = Mp4DetectionConfidence::Strong;
    } else if (evidence.size() == 2U) {
        candidate.confidence = Mp4DetectionConfidence::Probable;
    } else {
        candidate.confidence = Mp4DetectionConfidence::Weak;
    }

    candidate.evidence = std::move(evidence);
    result.candidate = std::move(candidate);
    return result;
}

} // namespace streamview::rules
