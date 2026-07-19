#pragma once

#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>

#include <QString>
#include <QtGlobal>

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
    BitReader(const RandomAccessSource& source, SourceSpan range) noexcept
        : source_(&source), range_(range) {}

    [[nodiscard]] quint64 position() const noexcept { return position_; }
    [[nodiscard]] quint64 remainingBits() const noexcept {
        return range_.bitLength() - position_;
    }
    [[nodiscard]] const SourceSpan& range() const noexcept { return range_; }

    [[nodiscard]] bool seek(quint64 bitOffset) noexcept;
    [[nodiscard]] BitReadResult readBits(unsigned int bitCount);

private:
    const RandomAccessSource* source_ = nullptr;
    SourceSpan range_;
    quint64 position_ = 0;
};

} // namespace streamview::core
