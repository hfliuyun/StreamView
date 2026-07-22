#include <streamview/rules/h264_annex_b_detector.h>

#include "h264_start_code_prefix.h"

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

} // namespace

H264AnnexBDetectionResult
detectH264AnnexBCandidate(std::span<const std::byte> sourcePrefix,
                          quint64 sourceSizeBytes) {
    H264AnnexBDetectionResult result;
    const quint64 suppliedBytes = static_cast<quint64>(sourcePrefix.size());
    result.inspectedByteCount =
        std::min({suppliedBytes, sourceSizeBytes, h264AnnexBDetectionProbeSizeBytes()});
    result.sourceFullyInspected = result.inspectedByteCount == sourceSizeBytes;

    const auto inspectedSize = static_cast<std::size_t>(result.inspectedByteCount);
    H264AnnexBCandidate candidate;
    std::size_t validHeaderCount = 0;
    for (std::size_t offset = 0; offset + 2U < inspectedSize; ++offset) {
        const auto first = std::to_integer<quint8>(sourcePrefix[offset]);
        const auto second = std::to_integer<quint8>(sourcePrefix[offset + 1U]);
        const auto third = std::to_integer<quint8>(sourcePrefix[offset + 2U]);
        if (first != 0U || second != 0U) {
            continue;
        }

        std::optional<quint8> fourth;
        if (offset + 3U < inspectedSize) {
            fourth = std::to_integer<quint8>(sourcePrefix[offset + 3U]);
        }
        const quint8 startCodeLength =
            detail::h264StartCodePrefixLength(first, second, third, fourth);
        if (startCodeLength == 0U) {
            continue;
        }

        H264AnnexBDetectionEvidence evidence;
        const quint64 byteOffset = static_cast<quint64>(offset);
        evidence.startCode = byteSpan(byteOffset, startCodeLength);
        const std::size_t headerOffset = offset + startCodeLength;
        if (headerOffset < inspectedSize) {
            const quint8 header = std::to_integer<quint8>(sourcePrefix[headerOffset]);
            evidence.nalUnitHeader = byteSpan(static_cast<quint64>(headerOffset), 1);
            evidence.nalUnitType = static_cast<quint8>(header & 0x1FU);
            evidence.forbiddenZeroBitIsZero = (header & 0x80U) == 0U;
        }

        if (evidence.nalUnitHeader && evidence.forbiddenZeroBitIsZero &&
            evidence.nalUnitType && *evidence.nalUnitType >= 1U &&
            *evidence.nalUnitType <= 23U) {
            ++validHeaderCount;
        }
        candidate.evidence.push_back(std::move(evidence));
        offset += static_cast<std::size_t>(startCodeLength) - 1U;
    }

    if (!candidate.evidence.empty()) {
        if (validHeaderCount >= 2U) {
            candidate.confidence = H264AnnexBDetectionConfidence::Strong;
        } else if (validHeaderCount == 1U) {
            candidate.confidence = H264AnnexBDetectionConfidence::Probable;
        }
        result.candidate = std::move(candidate);
    }
    return result;
}

} // namespace streamview::rules
