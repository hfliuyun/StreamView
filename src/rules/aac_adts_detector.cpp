#include <streamview/rules/aac_adts_detector.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace streamview::rules {

namespace {

[[nodiscard]] std::optional<core::SourceSpan> byteSpan(quint64 offset,
                                                        quint64 byteLength) {
    if (offset > std::numeric_limits<quint64>::max() / 8U ||
        byteLength > std::numeric_limits<quint64>::max() / 8U) {
        return std::nullopt;
    }
    return core::SourceSpan::create(core::SourceBitAddress(offset * 8U),
                                    byteLength * 8U);
}

struct ParsedAdtsHeaderInfo {
    quint16 frameLength = 0;
    quint8 headerLength = 0;
    bool crcPresent = false;
    quint8 profile = 0;
    quint8 samplingFrequencyIndex = 0;
    quint8 channelConfiguration = 0;
};

[[nodiscard]] std::optional<ParsedAdtsHeaderInfo>
parseHeaderAt(std::span<const std::byte> prefix, std::size_t offset) {
    if (offset + 7U > prefix.size()) {
        return std::nullopt;
    }
    const auto b0 = std::to_integer<quint8>(prefix[offset]);
    const auto b1 = std::to_integer<quint8>(prefix[offset + 1U]);
    if (b0 != 0xFFU || (b1 & 0xF6U) != 0xF0U) {
        return std::nullopt;
    }
    const auto b2 = std::to_integer<quint8>(prefix[offset + 2U]);
    const auto b3 = std::to_integer<quint8>(prefix[offset + 3U]);
    const auto b4 = std::to_integer<quint8>(prefix[offset + 4U]);
    const auto b5 = std::to_integer<quint8>(prefix[offset + 5U]);

    const bool crcPresent = (b1 & 0x01U) == 0U;
    const quint8 headerLength = crcPresent ? 9U : 7U;
    const quint16 frameLength = static_cast<quint16>(
        (static_cast<quint32>(b3 & 0x03U) << 11U) |
        (static_cast<quint32>(b4) << 3U) |
        (static_cast<quint32>(b5 >> 5U) & 0x07U));
    if (frameLength < headerLength) {
        return std::nullopt;
    }
    const quint8 profile = static_cast<quint8>((b2 >> 6U) & 0x03U);
    const quint8 samplingFreqIndex = static_cast<quint8>((b2 >> 2U) & 0x0FU);
    const quint8 channelConfig = static_cast<quint8>(
        ((b2 & 0x01U) << 2U) | ((b3 >> 6U) & 0x03U));

    ParsedAdtsHeaderInfo info;
    info.frameLength = frameLength;
    info.headerLength = headerLength;
    info.crcPresent = crcPresent;
    info.profile = profile;
    info.samplingFrequencyIndex = samplingFreqIndex;
    info.channelConfiguration = channelConfig;
    return info;
}

} // namespace

AacAdtsDetectionResult
detectAacAdtsCandidate(std::span<const std::byte> sourcePrefix,
                       quint64 sourceSizeBytes) {
    AacAdtsDetectionResult result;
    const quint64 suppliedBytes = static_cast<quint64>(sourcePrefix.size());
    result.inspectedByteCount =
        std::min({suppliedBytes, sourceSizeBytes, aacAdtsDetectionProbeSizeBytes()});
    result.sourceFullyInspected = result.inspectedByteCount == sourceSizeBytes;

    const auto inspectedSize = static_cast<std::size_t>(result.inspectedByteCount);
    AacAdtsCandidate candidate;

    std::size_t maxChainLength = 0;

    std::size_t offset = 0;
    while (offset + 7U <= inspectedSize) {
        const auto header = parseHeaderAt(sourcePrefix, offset);
        if (!header.has_value()) {
            ++offset;
            continue;
        }

        // Trace length chain starting from this offset
        std::size_t chainLength = 0;
        std::size_t chainOffset = offset;
        while (chainOffset + 7U <= inspectedSize) {
            const auto chainHeader = parseHeaderAt(sourcePrefix, chainOffset);
            if (!chainHeader.has_value()) {
                break;
            }
            ++chainLength;
            AacAdtsDetectionEvidence evidence;
            evidence.syncword = byteSpan(static_cast<quint64>(chainOffset), 2);
            evidence.header = byteSpan(static_cast<quint64>(chainOffset), chainHeader->headerLength);
            evidence.aacFrameLength = chainHeader->frameLength;
            evidence.crcPresent = chainHeader->crcPresent;
            evidence.profile = chainHeader->profile;
            evidence.samplingFrequencyIndex = chainHeader->samplingFrequencyIndex;
            evidence.channelConfiguration = chainHeader->channelConfiguration;
            candidate.evidence.push_back(std::move(evidence));

            if (chainOffset + chainHeader->frameLength >= inspectedSize) {
                // Next frame is beyond inspected range
                chainOffset += chainHeader->frameLength;
                break;
            }
            chainOffset += chainHeader->frameLength;
        }

        if (chainLength > maxChainLength) {
            maxChainLength = chainLength;
        }

        if (chainLength > 1) {
            offset = chainOffset;
        } else {
            ++offset;
        }
    }

    if (!candidate.evidence.empty()) {
        if (maxChainLength >= 3U) {
            candidate.confidence = AacAdtsDetectionConfidence::Strong;
        } else if (maxChainLength == 2U) {
            candidate.confidence = AacAdtsDetectionConfidence::Probable;
        } else {
            candidate.confidence = AacAdtsDetectionConfidence::Weak;
        }
        result.candidate = std::move(candidate);
    }
    return result;
}

} // namespace streamview::rules
