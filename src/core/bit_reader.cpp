#include <streamview/core/bit_reader.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace streamview::core {

namespace {

[[nodiscard]] BitReadResult readContiguousBits(const RandomAccessSource& source,
                                               quint64 absoluteStart,
                                               unsigned int bitCount) {
    const quint64 absoluteEnd = absoluteStart + static_cast<quint64>(bitCount) - 1;
    const quint64 startByte = absoluteStart / 8;
    const quint64 endByte = absoluteEnd / 8;
    const auto byteCount = static_cast<std::size_t>((endByte - startByte) + 1);

    std::array<std::byte, 9> bytes{};
    const SourceReadResult sourceResult =
        source.readAt(startByte, std::span<std::byte>(bytes.data(), byteCount));
    if (!sourceResult.complete() || sourceResult.bytesRead != byteCount) {
        if (sourceResult.status == SourceReadStatus::EndOfSource) {
            return {BitReadStatus::EndOfSource, 0, 0, {}};
        }
        if (sourceResult.complete()) {
            return {BitReadStatus::SourceError,
                    0,
                    0,
                    QStringLiteral("Source reported an incomplete successful read")};
        }
        return {BitReadStatus::SourceError, 0, 0, sourceResult.errorMessage};
    }

    quint64 value = 0;
    for (unsigned int index = 0; index < bitCount; ++index) {
        const quint64 absoluteBit = absoluteStart + index;
        const auto byteIndex = static_cast<std::size_t>((absoluteBit / 8) - startByte);
        const unsigned int bitInByte = static_cast<unsigned int>(absoluteBit % 8);
        const auto byteValue = static_cast<unsigned int>(bytes.at(byteIndex));
        const quint64 bit = (byteValue >> (7U - bitInByte)) & 1U;
        value = (value << 1U) | bit;
    }

    return {BitReadStatus::Complete, value, static_cast<quint8>(bitCount), {}};
}

} // namespace

BitReader::BitReader(const RandomAccessSource& source, SourceSpan range)
    : BitReader(source, std::vector<SourceSpan>{range}, range.bitLength()) {}

BitReader::BitReader(const RandomAccessSource& source, const SourceMapping& mapping)
    : BitReader(source, mapping.sourceSpans(), mapping.logicalBitLength()) {}

std::optional<BitReader> BitReader::fromMappingSlice(const RandomAccessSource& source,
                                                     const SourceMapping& mapping,
                                                     quint64 logicalStart,
                                                     quint64 logicalBitLength) {
    const auto range = LogicalRange::create(
        LogicalBitAddress(mapping.viewId(), logicalStart), logicalBitLength);
    const auto location = range ? mapping.locate(*range) : std::nullopt;
    if (!location) {
        return std::nullopt;
    }
    return BitReader(source, location->sourceSpans(), logicalBitLength);
}

BitReader::BitReader(const RandomAccessSource& source,
                     std::vector<SourceSpan> backingSpans,
                     quint64 logicalBitLength)
    : source_(&source), backingSpans_(std::move(backingSpans)),
      logicalBitLength_(logicalBitLength) {
    indexBackingSpans();
}

void BitReader::indexBackingSpans() {
    logicalSpanStarts_.reserve(backingSpans_.size());
    quint64 logicalStart = 0;
    for (const SourceSpan& span : backingSpans_) {
        logicalSpanStarts_.push_back(logicalStart);
        logicalStart += span.bitLength();
    }
}

bool BitReader::seek(quint64 bitOffset) noexcept {
    if (bitOffset > logicalBitLength_) {
        return false;
    }
    position_ = bitOffset;
    return true;
}

BitReadResult BitReader::readBits(unsigned int bitCount) {
    if (bitCount == 0 || bitCount > 64) {
        return {BitReadStatus::InvalidBitCount, 0, 0, QStringLiteral("Bit count must be 1..64")};
    }
    if (static_cast<quint64>(bitCount) > remainingBits()) {
        return {BitReadStatus::EndOfRange, 0, 0, {}};
    }

    const auto nextSpan = std::upper_bound(
        logicalSpanStarts_.begin(), logicalSpanStarts_.end(), position_);
    if (nextSpan == logicalSpanStarts_.begin()) {
        return {BitReadStatus::SourceError,
                0,
                0,
                QStringLiteral("Reader backing does not cover its logical range")};
    }

    std::size_t spanIndex =
        static_cast<std::size_t>(std::distance(logicalSpanStarts_.begin(), nextSpan) - 1);
    quint64 logicalPosition = position_;
    unsigned int bitsRemaining = bitCount;
    quint64 value = 0;
    while (bitsRemaining != 0 && spanIndex < backingSpans_.size()) {
        const SourceSpan& span = backingSpans_.at(spanIndex);
        const quint64 offsetInSpan = logicalPosition - logicalSpanStarts_.at(spanIndex);
        const quint64 availableInSpan = span.bitLength() - offsetInSpan;
        const auto chunkBits = static_cast<unsigned int>(
            std::min<quint64>(availableInSpan, bitsRemaining));
        const quint64 absoluteStart = span.start().absoluteBitOffset() + offsetInSpan;
        const BitReadResult chunk = readContiguousBits(*source_, absoluteStart, chunkBits);
        if (!chunk.complete()) {
            return chunk;
        }
        value = chunkBits == 64 ? chunk.value : (value << chunkBits) | chunk.value;
        logicalPosition += chunkBits;
        bitsRemaining -= chunkBits;
        ++spanIndex;
    }
    if (bitsRemaining != 0) {
        return {BitReadStatus::SourceError,
                0,
                0,
                QStringLiteral("Reader backing does not cover its logical range")};
    }

    position_ += bitCount;
    return {BitReadStatus::Complete, value, static_cast<quint8>(bitCount), {}};
}

} // namespace streamview::core
