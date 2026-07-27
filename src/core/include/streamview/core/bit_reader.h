#pragma once

#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>

#include <QString>
#include <QtGlobal>

#include <optional>
#include <vector>

namespace streamview::core {

enum class BitReadStatus : quint8 {
    Complete,
    InvalidBitCount,
    EndOfRange,
    EndOfSource,
    SourceError,
};

struct BitReadResult final {
    BitReadStatus status = BitReadStatus::SourceError;
    quint64 value = 0;
    quint8 bitCount = 0;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept { return status == BitReadStatus::Complete; }
};

class BitReader final {
public:
    BitReader(const RandomAccessSource& source, SourceSpan range);
    BitReader(const RandomAccessSource& source, const SourceMapping& mapping);

    [[nodiscard]] static std::optional<BitReader>
    fromMappingSlice(const RandomAccessSource& source,
                     const SourceMapping& mapping,
                     quint64 logicalStart,
                     quint64 logicalBitLength);

    [[nodiscard]] quint64 position() const noexcept { return position_; }
    [[nodiscard]] quint64 remainingBits() const noexcept {
        return logicalBitLength_ - position_;
    }
    [[nodiscard]] quint64 logicalBitLength() const noexcept { return logicalBitLength_; }
    [[nodiscard]] const std::vector<SourceSpan>& backingSpans() const noexcept {
        return backingSpans_;
    }

    [[nodiscard]] bool seek(quint64 bitOffset) noexcept;
    [[nodiscard]] BitReadResult readBits(unsigned int bitCount);

private:
    BitReader(const RandomAccessSource& source,
              std::vector<SourceSpan> backingSpans,
              quint64 logicalBitLength);

    void indexBackingSpans();

    const RandomAccessSource* source_ = nullptr;
    std::vector<SourceSpan> backingSpans_;
    std::vector<quint64> logicalSpanStarts_;
    quint64 logicalBitLength_ = 0;
    quint64 position_ = 0;
};

} // namespace streamview::core
