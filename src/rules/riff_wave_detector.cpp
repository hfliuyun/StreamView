#include <streamview/rules/riff_wave_detector.h>

#include <algorithm>
#include <limits>

namespace streamview::rules {

namespace {

constexpr quint32 kRiffMagic = 0x52494646U;   // 'RIFF'
constexpr quint32 kWaveForm = 0x57415645U;    // 'WAVE'

[[nodiscard]] quint32 readBigEndianU32(const std::byte* bytes) noexcept {
    return (static_cast<quint32>(static_cast<quint8>(bytes[0])) << 24) |
           (static_cast<quint32>(static_cast<quint8>(bytes[1])) << 16) |
           (static_cast<quint32>(static_cast<quint8>(bytes[2])) << 8) |
           static_cast<quint32>(static_cast<quint8>(bytes[3]));
}

[[nodiscard]] quint32 readLittleEndianU32(const std::byte* bytes) noexcept {
    return (static_cast<quint32>(static_cast<quint8>(bytes[3])) << 24) |
           (static_cast<quint32>(static_cast<quint8>(bytes[2])) << 16) |
           (static_cast<quint32>(static_cast<quint8>(bytes[1])) << 8) |
           static_cast<quint32>(static_cast<quint8>(bytes[0]));
}

[[nodiscard]] bool checkBitCoordinateOverflow(quint64 offsetBytes, quint64 lengthBytes) noexcept {
    constexpr quint64 maxBytes = std::numeric_limits<quint64>::max() / 8U;
    if (offsetBytes > maxBytes || lengthBytes > maxBytes) {
        return false;
    }
    return lengthBytes <= (maxBytes - offsetBytes);
}

} // namespace

RiffWaveDetectionResult
detectRiffWaveCandidate(std::span<const std::byte> sourcePrefix,
                        quint64 sourceSizeBytes) {
    const quint64 prefixSize = static_cast<quint64>(sourcePrefix.size());
    const quint64 probeMax = riffWaveDetectionProbeSizeBytes();
    const quint64 inspectionSize = std::min({prefixSize, sourceSizeBytes, probeMax});

    RiffWaveDetectionResult result;
    result.inspectedByteCount = inspectionSize;
    result.sourceFullyInspected = (inspectionSize >= sourceSizeBytes);

    if (sourcePrefix.empty() || sourceSizeBytes == 0U || inspectionSize < 12U) {
        return result;
    }

    const std::byte* headerPtr = sourcePrefix.data();
    if (readBigEndianU32(headerPtr) != kRiffMagic ||
        readBigEndianU32(headerPtr + 8U) != kWaveForm) {
        return result;
    }

    const quint64 riffSize = static_cast<quint64>(readLittleEndianU32(headerPtr + 4U));

    std::vector<RiffWaveDetectionEvidence> evidence;
    if (!checkBitCoordinateOverflow(0U, 12U)) {
        return result;
    }
    const auto headerSpan = core::SourceSpan::create(
        core::SourceBitAddress(0U), 12U * 8U);
    if (!headerSpan.has_value()) {
        return result;
    }
    RiffWaveDetectionEvidence headerEvidence;
    headerEvidence.chunkSpan = headerSpan;
    headerEvidence.chunkOffset = 0U;
    headerEvidence.chunkId = kRiffMagic;
    headerEvidence.declaredSize = riffSize;
    headerEvidence.isFormHeader = true;
    evidence.push_back(headerEvidence);

    if (riffSize < 4U) {
        // A degenerate form size frames no chunks.
        RiffWaveCandidate candidate;
        candidate.confidence = RiffWaveDetectionConfidence::Weak;
        candidate.evidence = std::move(evidence);
        result.candidate = std::move(candidate);
        return result;
    }

    const quint64 formEnd = 8U + riffSize;
    quint64 offset = 12U;

    while (offset < inspectionSize && offset < formEnd) {
        const quint64 remainingInInspection = inspectionSize - offset;
        if (remainingInInspection < 8U) {
            break;
        }

        const std::byte* chunkPtr = sourcePrefix.data() + offset;
        const quint32 chunkId = readBigEndianU32(chunkPtr);
        const quint64 chunkSize = static_cast<quint64>(readLittleEndianU32(chunkPtr + 4U));
        // 8 (header) + size (<= 0xFFFFFFFF) + 1 (pad) cannot overflow quint64.
        const quint64 chunkTotal = 8U + chunkSize + (chunkSize & 1U);

        if (!checkBitCoordinateOverflow(offset, chunkTotal)) {
            break;
        }

        if (chunkTotal <= remainingInInspection) {
            const auto span = core::SourceSpan::create(
                core::SourceBitAddress(offset * 8U), chunkTotal * 8U);
            if (!span.has_value()) {
                break;
            }
            RiffWaveDetectionEvidence ev;
            ev.chunkSpan = span;
            ev.chunkOffset = offset;
            ev.chunkId = chunkId;
            ev.declaredSize = chunkSize;
            ev.isFormHeader = false;
            evidence.push_back(ev);

            offset += chunkTotal;
            continue;
        }

        // The chunk body extends past the inspection window. When it still
        // fits in the source, the verified header alone is evidence;
        // otherwise the chunk is truncated in the source and adds nothing.
        const quint64 remainingInSource = sourceSizeBytes - offset;
        if (chunkTotal > remainingInSource) {
            break;
        }
        const auto span = core::SourceSpan::create(
            core::SourceBitAddress(offset * 8U), 8U * 8U);
        if (!span.has_value()) {
            break;
        }
        RiffWaveDetectionEvidence ev;
        ev.chunkSpan = span;
        ev.chunkOffset = offset;
        ev.chunkId = chunkId;
        ev.declaredSize = chunkSize;
        ev.isFormHeader = false;
        evidence.push_back(ev);
        break;
    }

    RiffWaveCandidate candidate;
    if (evidence.size() >= 3U) {
        candidate.confidence = RiffWaveDetectionConfidence::Strong;
    } else if (evidence.size() == 2U) {
        candidate.confidence = RiffWaveDetectionConfidence::Probable;
    } else {
        candidate.confidence = RiffWaveDetectionConfidence::Weak;
    }
    candidate.evidence = std::move(evidence);
    result.candidate = std::move(candidate);
    return result;
}

} // namespace streamview::rules
