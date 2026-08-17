#include <streamview/rules/mp4_box_scanner.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace streamview::rules {

namespace {

constexpr quint64 kChunkSize = 64U * 1024U;

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

Mp4BoxScanner::ReadBytesStatus Mp4BoxScanner::readBytes(quint64 offset,
                                                        std::size_t count,
                                                        std::byte* destination,
                                                        QString* errorMessage) {
    if (source_ == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Source is not available");
        }
        return ReadBytesStatus::Error;
    }

    const quint64 sourceSize = source_->sizeBytes();
    if (offset >= sourceSize) {
        return ReadBytesStatus::End;
    }
    if (count > (sourceSize - offset)) {
        return ReadBytesStatus::End;
    }

    if (offset < bufferStart_ || (offset + count) > bufferEnd_) {
        bufferStart_ = (offset / kChunkSize) * kChunkSize;
        const quint64 available = sourceSize - bufferStart_;
        const quint64 requested = std::min(kChunkSize, available);
        if (requested > static_cast<quint64>(std::numeric_limits<std::size_t>::max())) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Scanner chunk exceeds host size limits");
            }
            return ReadBytesStatus::Error;
        }

        buffer_.resize(static_cast<std::size_t>(requested));
        const auto readResult = source_->readAt(
            bufferStart_, std::span<std::byte>(buffer_.data(), buffer_.size()));
        if (!readResult.complete() || readResult.bytesRead != buffer_.size()) {
            if (errorMessage != nullptr) {
                *errorMessage = readResult.errorMessage.isEmpty()
                                    ? QStringLiteral("Unable to read source while scanning MP4 boxes")
                                    : readResult.errorMessage;
            }
            return ReadBytesStatus::Error;
        }
        bufferEnd_ = bufferStart_ + requested;
    }

    const std::size_t bufferOffset = static_cast<std::size_t>(offset - bufferStart_);
    std::memcpy(destination, buffer_.data() + bufferOffset, count);
    return ReadBytesStatus::Available;
}

Mp4BoxScanBatch Mp4BoxScanner::scanBatch(std::size_t maximumRecords,
                                         quint64 maximumInspectedPositions) {
    Mp4BoxScanBatch batch;

    if (maximumRecords == 0) {
        batch.status = Mp4BoxScanStatus::InvalidBatchSize;
        batch.errorMessage = QStringLiteral("Batch maximum record count must be greater than zero");
        return batch;
    }

    if (source_ == nullptr) {
        batch.status = Mp4BoxScanStatus::SourceError;
        batch.errorMessage = QStringLiteral("Source is not available");
        return batch;
    }

    const quint64 sourceSize = source_->sizeBytes();
    if (finished_ || cursor_ >= sourceSize) {
        finished_ = true;
        batch.status = Mp4BoxScanStatus::Complete;
        return batch;
    }

    if (cancellation_.has_value() && cancellation_->isCancellationRequested()) {
        batch.status = Mp4BoxScanStatus::Cancelled;
        return batch;
    }

    batch.status = Mp4BoxScanStatus::InProgress;
    inspectedPositions_ = 0;

    while (batch.records.size() < maximumRecords &&
           inspectedPositions_ < maximumInspectedPositions &&
           cursor_ < sourceSize) {
        if (cancellation_.has_value() && cancellation_->isCancellationRequested()) {
            batch.status = Mp4BoxScanStatus::Cancelled;
            return batch;
        }

        const quint64 start = cursor_;
        const quint64 remaining = sourceSize - start;

        if (remaining < 8U) {
            finished_ = true;
            batch.status = Mp4BoxScanStatus::Complete;
            return batch;
        }

        ++inspectedPositions_;

        std::byte headerBytes[8];
        const auto headerStatus = readBytes(start, 8U, headerBytes, &batch.errorMessage);
        if (headerStatus == ReadBytesStatus::Error) {
            batch.status = Mp4BoxScanStatus::SourceError;
            return batch;
        }
        if (headerStatus == ReadBytesStatus::End) {
            finished_ = true;
            batch.status = Mp4BoxScanStatus::Complete;
            return batch;
        }

        const quint32 size = readBigEndianU32(headerBytes);

        if (size == 0U) {
            if (!checkBitCoordinateOverflow(start, remaining)) {
                batch.status = Mp4BoxScanStatus::SourceError;
                batch.errorMessage = QStringLiteral("Coordinate arithmetic overflow");
                return batch;
            }

            const auto span = core::SourceSpan::create(
                core::SourceBitAddress(start * 8U), remaining * 8U);
            if (!span.has_value()) {
                batch.status = Mp4BoxScanStatus::SourceError;
                batch.errorMessage = QStringLiteral("Failed to create source span");
                return batch;
            }

            Mp4BoxRecord record;
            record.boxSpan = span;
            record.boxOffset = start;
            record.declaredBoxSize = 0U;
            record.truncated = false;
            record.terminal = true;
            batch.records.push_back(record);

            cursor_ = sourceSize;
            finished_ = true;
            batch.status = Mp4BoxScanStatus::Complete;
            return batch;
        }

        quint64 boxSize = 0;
        quint64 declaredSize = 0;

        if (size == 1U) {
            if (remaining < 16U) {
                finished_ = true;
                batch.status = Mp4BoxScanStatus::Complete;
                return batch;
            }

            std::byte largeSizeBytes[8];
            const auto largeStatus = readBytes(start + 8U, 8U, largeSizeBytes, &batch.errorMessage);
            if (largeStatus == ReadBytesStatus::Error) {
                batch.status = Mp4BoxScanStatus::SourceError;
                return batch;
            }
            if (largeStatus == ReadBytesStatus::End) {
                finished_ = true;
                batch.status = Mp4BoxScanStatus::Complete;
                return batch;
            }

            const quint64 largesize = readBigEndianU64(largeSizeBytes);
            if (largesize < 16U) {
                finished_ = true;
                batch.status = Mp4BoxScanStatus::Complete;
                return batch;
            }

            boxSize = largesize;
            declaredSize = largesize;
        } else if (size >= 8U) {
            boxSize = size;
            declaredSize = size;
        } else {
            finished_ = true;
            batch.status = Mp4BoxScanStatus::Complete;
            return batch;
        }

        if (boxSize <= remaining) {
            if (!checkBitCoordinateOverflow(start, boxSize)) {
                batch.status = Mp4BoxScanStatus::SourceError;
                batch.errorMessage = QStringLiteral("Coordinate arithmetic overflow");
                return batch;
            }

            const auto span = core::SourceSpan::create(
                core::SourceBitAddress(start * 8U), boxSize * 8U);
            if (!span.has_value()) {
                batch.status = Mp4BoxScanStatus::SourceError;
                batch.errorMessage = QStringLiteral("Failed to create source span");
                return batch;
            }

            Mp4BoxRecord record;
            record.boxSpan = span;
            record.boxOffset = start;
            record.declaredBoxSize = declaredSize;
            record.truncated = false;
            record.terminal = false;
            batch.records.push_back(record);

            cursor_ = start + boxSize;
            if (cursor_ >= sourceSize) {
                finished_ = true;
            }
        } else {
            if (!checkBitCoordinateOverflow(start, remaining)) {
                batch.status = Mp4BoxScanStatus::SourceError;
                batch.errorMessage = QStringLiteral("Coordinate arithmetic overflow");
                return batch;
            }

            const auto span = core::SourceSpan::create(
                core::SourceBitAddress(start * 8U), remaining * 8U);
            if (!span.has_value()) {
                batch.status = Mp4BoxScanStatus::SourceError;
                batch.errorMessage = QStringLiteral("Failed to create source span");
                return batch;
            }

            Mp4BoxRecord record;
            record.boxSpan = span;
            record.boxOffset = start;
            record.declaredBoxSize = declaredSize;
            record.truncated = true;
            record.terminal = true;
            batch.records.push_back(record);

            cursor_ = sourceSize;
            finished_ = true;
            batch.status = Mp4BoxScanStatus::Complete;
            return batch;
        }
    }

    if (cursor_ >= sourceSize) {
        finished_ = true;
        batch.status = Mp4BoxScanStatus::Complete;
    } else if (batch.records.size() >= maximumRecords ||
               inspectedPositions_ >= maximumInspectedPositions) {
        batch.status = Mp4BoxScanStatus::InProgress;
    }

    return batch;
}

} // namespace streamview::rules
