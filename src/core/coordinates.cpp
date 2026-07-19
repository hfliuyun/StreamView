#include <streamview/core/coordinates.h>

#include <algorithm>
#include <limits>

namespace streamview::core {

namespace {

[[nodiscard]] bool addWouldOverflow(quint64 left, quint64 right) noexcept {
    return right > std::numeric_limits<quint64>::max() - left;
}

} // namespace

std::optional<SourceBitAddress>
SourceBitAddress::fromByteAndBit(quint64 byteOffset, quint8 bitOffsetInByte) noexcept {
    if (bitOffsetInByte >= 8) {
        return std::nullopt;
    }

    constexpr quint64 max = std::numeric_limits<quint64>::max();
    if (byteOffset > (max - bitOffsetInByte) / 8) {
        return std::nullopt;
    }

    return SourceBitAddress((byteOffset * 8) + bitOffsetInByte);
}

std::optional<SourceSpan> SourceSpan::create(SourceBitAddress start, quint64 bitLength) noexcept {
    if (addWouldOverflow(start.absoluteBitOffset(), bitLength)) {
        return std::nullopt;
    }
    return SourceSpan(start, bitLength);
}

std::optional<LogicalRange>
LogicalRange::create(LogicalBitAddress start, quint64 bitLength) noexcept {
    if (addWouldOverflow(start.bitOffset(), bitLength)) {
        return std::nullopt;
    }
    return LogicalRange(start, bitLength);
}

std::optional<SourceMapping>
SourceMapping::create(LogicalViewId viewId, std::vector<SourceSpan> sourceSpans) {
    quint64 logicalBitLength = 0;
    std::vector<SourceSpan> normalized;
    normalized.reserve(sourceSpans.size());

    for (const SourceSpan& span : sourceSpans) {
        if (span.bitLength() == 0 || addWouldOverflow(logicalBitLength, span.bitLength())) {
            return std::nullopt;
        }

        if (!normalized.empty()) {
            const SourceSpan& previous = normalized.back();
            const quint64 previousEnd = previous.endExclusive().absoluteBitOffset();
            const quint64 currentStart = span.start().absoluteBitOffset();
            if (currentStart < previousEnd) {
                return std::nullopt;
            }
            if (currentStart == previousEnd) {
                const auto merged = SourceSpan::create(
                    previous.start(), previous.bitLength() + span.bitLength());
                if (!merged) {
                    return std::nullopt;
                }
                normalized.back() = *merged;
                logicalBitLength += span.bitLength();
                continue;
            }
        }

        normalized.push_back(span);
        logicalBitLength += span.bitLength();
    }

    return SourceMapping(viewId, logicalBitLength, std::move(normalized));
}

std::optional<FieldLocation> SourceMapping::locate(const LogicalRange& range) const {
    if (range.start().viewId() != viewId_ || range.endOffsetExclusive() > logicalBitLength_) {
        return std::nullopt;
    }

    std::vector<SourceSpan> resolved;
    if (range.bitLength() == 0) {
        return FieldLocation(range, std::move(resolved));
    }

    const quint64 requestedStart = range.start().bitOffset();
    const quint64 requestedEnd = range.endOffsetExclusive();
    quint64 logicalCursor = 0;

    for (const SourceSpan& span : sourceSpans_) {
        const quint64 spanLogicalEnd = logicalCursor + span.bitLength();
        const quint64 overlapStart = std::max(requestedStart, logicalCursor);
        const quint64 overlapEnd = std::min(requestedEnd, spanLogicalEnd);

        if (overlapStart < overlapEnd) {
            const quint64 sourceStart =
                span.start().absoluteBitOffset() + (overlapStart - logicalCursor);
            const auto resolvedSpan = SourceSpan::create(
                SourceBitAddress(sourceStart), overlapEnd - overlapStart);
            if (!resolvedSpan) {
                return std::nullopt;
            }
            resolved.push_back(*resolvedSpan);
        }

        logicalCursor = spanLogicalEnd;
        if (logicalCursor >= requestedEnd) {
            break;
        }
    }

    return FieldLocation(range, std::move(resolved));
}

} // namespace streamview::core
