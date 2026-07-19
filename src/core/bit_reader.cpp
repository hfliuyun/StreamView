#include <streamview/core/bit_reader.h>

#include <array>
#include <cstddef>
#include <span>

namespace streamview::core {

bool BitReader::seek(quint64 bitOffset) noexcept {
    if (bitOffset > range_.bitLength()) {
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

    const quint64 absoluteStart = range_.start().absoluteBitOffset() + position_;
    const quint64 absoluteEnd = absoluteStart + static_cast<quint64>(bitCount) - 1;
    const quint64 startByte = absoluteStart / 8;
    const quint64 endByte = absoluteEnd / 8;
    const auto byteCount = static_cast<std::size_t>((endByte - startByte) + 1);

    std::array<std::byte, 9> bytes{};
    const SourceReadResult sourceResult =
        source_->readAt(startByte, std::span<std::byte>(bytes.data(), byteCount));
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

    position_ += bitCount;
    return {BitReadStatus::Complete, value, static_cast<quint8>(bitCount), {}};
}

} // namespace streamview::core
