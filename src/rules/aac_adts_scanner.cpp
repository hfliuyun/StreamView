#include <streamview/rules/aac_adts_scanner.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>

namespace streamview::rules {

namespace {

constexpr quint64 kChunkSize = 64U * 1024U;

[[nodiscard]] std::optional<core::SourceSpan> makeByteSpan(quint64 offset,
                                                            quint64 byteLength) {
    if (byteLength > std::numeric_limits<quint64>::max() / 8U ||
        offset > std::numeric_limits<quint64>::max() / 8U) {
        return std::nullopt;
    }
    return core::SourceSpan::create(core::SourceBitAddress(offset * 8U), byteLength * 8U);
}

} // namespace

AacAdtsScanner::ReadByteStatus AacAdtsScanner::readByte(quint64 offset,
                                                         quint8* value,
                                                         QString* errorMessage) {
    if (offset >= source_->sizeBytes()) {
        return ReadByteStatus::End;
    }
    if (offset < bufferStart_ || offset >= bufferEnd_) {
        bufferStart_ = (offset / kChunkSize) * kChunkSize;
        const quint64 available = source_->sizeBytes() - bufferStart_;
        const quint64 requested = std::min(kChunkSize, available);
        if (requested > static_cast<quint64>(std::numeric_limits<std::size_t>::max())) {
            *errorMessage = QStringLiteral("Scanner chunk exceeds host size limits");
            return ReadByteStatus::Error;
        }
        buffer_.resize(static_cast<std::size_t>(requested));
        const core::SourceReadResult readResult = source_->readAt(
            bufferStart_, std::span<std::byte>(buffer_.data(), buffer_.size()));
        if (!readResult.complete() || readResult.bytesRead != buffer_.size()) {
            *errorMessage = readResult.errorMessage.isEmpty()
                                ? QStringLiteral("Unable to read source while scanning ADTS frames")
                                : readResult.errorMessage;
            return ReadByteStatus::Error;
        }
        bufferEnd_ = bufferStart_ + requested;
    }
    const auto index = static_cast<std::size_t>(offset - bufferStart_);
    *value = std::to_integer<quint8>(buffer_.at(index));
    return ReadByteStatus::Available;
}

std::optional<AacAdtsScanner::ParsedHeader>
AacAdtsScanner::parseHeaderAt(quint64 offset, QString* errorMessage) {
    if (offset + 7U > source_->sizeBytes()) {
        return std::nullopt;
    }
    quint8 bytes[7] = {};
    for (std::size_t i = 0; i < 7; ++i) {
        if (readByte(offset + i, &bytes[i], errorMessage) != ReadByteStatus::Available) {
            return std::nullopt;
        }
    }
    if (bytes[0] != 0xFFU || (bytes[1] & 0xF6U) != 0xF0U) {
        return std::nullopt;
    }
    const bool crcPresent = (bytes[1] & 0x01U) == 0U;
    const quint8 headerLength = crcPresent ? 9U : 7U;
    const quint16 frameLength = static_cast<quint16>(
        (static_cast<quint32>(bytes[3] & 0x03U) << 11U) |
        (static_cast<quint32>(bytes[4]) << 3U) |
        (static_cast<quint32>(bytes[5] >> 5U) & 0x07U));
    if (frameLength < headerLength) {
        return std::nullopt;
    }
    ParsedHeader header;
    header.frameLength = frameLength;
    header.headerLength = headerLength;
    header.crcPresent = crcPresent;
    return header;
}

bool AacAdtsScanner::verifyLookahead(quint64 offset,
                                    const ParsedHeader& header,
                                    QString* errorMessage) {
    const quint64 nextOffset1 = offset + header.frameLength;
    if (nextOffset1 >= source_->sizeBytes()) {
        return true;
    }
    if (nextOffset1 + 7U > source_->sizeBytes()) {
        quint8 b0 = 0;
        quint8 b1 = 0;
        if (readByte(nextOffset1, &b0, errorMessage) == ReadByteStatus::Available &&
            readByte(nextOffset1 + 1U, &b1, errorMessage) == ReadByteStatus::Available) {
            return b0 == 0xFFU && (b1 & 0xF6U) == 0xF0U;
        }
        return true;
    }
    const auto nextHeader1 = parseHeaderAt(nextOffset1, errorMessage);
    if (!nextHeader1) {
        return false;
    }
    const quint64 nextOffset2 = nextOffset1 + nextHeader1->frameLength;
    if (nextOffset2 >= source_->sizeBytes()) {
        return true;
    }
    quint8 b0 = 0;
    quint8 b1 = 0;
    if (readByte(nextOffset2, &b0, errorMessage) == ReadByteStatus::Available &&
        readByte(nextOffset2 + 1U, &b1, errorMessage) == ReadByteStatus::Available) {
        return b0 == 0xFFU && (b1 & 0xF6U) == 0xF0U;
    }
    return true;
}

std::optional<AacAdtsRecord>
AacAdtsScanner::makeRecord(quint64 offset,
                           const ParsedHeader& header,
                           QString* errorMessage) {
    if (offset > std::numeric_limits<quint64>::max() / 8U) {
        *errorMessage = QStringLiteral("Frame offset exceeds bit coordinate limits");
        return std::nullopt;
    }
    AacAdtsRecord record;
    record.frameOffset = offset;
    record.headerLength = header.headerLength;
    record.aacFrameLength = header.frameLength;
    record.crcPresent = header.crcPresent;

    const quint64 sourceSize = source_->sizeBytes();
    if (offset + header.frameLength <= sourceSize) {
        record.frameSpan = makeByteSpan(offset, header.frameLength);
        record.headerSpan = makeByteSpan(offset, header.headerLength);
        record.payloadLength = header.frameLength - header.headerLength;
        if (record.payloadLength > 0) {
            record.payloadSpan = makeByteSpan(offset + header.headerLength, record.payloadLength);
        }
        record.truncated = false;
    } else {
        const quint64 availableBytes = sourceSize - offset;
        record.frameSpan = makeByteSpan(offset, availableBytes);
        const quint64 availableHeader = std::min(availableBytes, static_cast<quint64>(header.headerLength));
        record.headerSpan = makeByteSpan(offset, availableHeader);
        if (availableBytes > header.headerLength) {
            record.payloadLength = availableBytes - header.headerLength;
            record.payloadSpan = makeByteSpan(offset + header.headerLength, record.payloadLength);
        } else {
            record.payloadLength = 0;
        }
        record.truncated = true;
    }

    if (!record.frameSpan || !record.headerSpan || (record.payloadLength > 0 && !record.payloadSpan)) {
        *errorMessage = QStringLiteral("ADTS frame span exceeds bit coordinate limits");
        return std::nullopt;
    }
    return record;
}

AacAdtsScanBatch AacAdtsScanner::scanBatch(std::size_t maximumRecords,
                                           quint64 maximumInspectedPositions) {
    AacAdtsScanBatch result;
    if (maximumRecords == 0 || maximumInspectedPositions == 0) {
        result.status = AacAdtsScanStatus::InvalidBatchSize;
        result.errorMessage =
            QStringLiteral("Maximum scan records and inspected positions must be greater than zero");
        return result;
    }
    if (source_->sizeBytes() > std::numeric_limits<quint64>::max() / 8U) {
        failed_ = true;
        result.status = AacAdtsScanStatus::SourceError;
        result.errorMessage =
            QStringLiteral("Source size exceeds the bit coordinate representation limit");
        return result;
    }
    if (failed_) {
        result.status = AacAdtsScanStatus::SourceError;
        result.errorMessage = QStringLiteral("ADTS scanner is in a failed state");
        return result;
    }
    if (finished_) {
        result.status = AacAdtsScanStatus::Complete;
        return result;
    }

    QString errorMessage;
    quint64 inspectedPositions = 0;
    while (!finished_ && result.records.size() < maximumRecords &&
           inspectedPositions < maximumInspectedPositions) {
        if (cancellation_ && (inspectedBytes_ % 1024U == 0) &&
            cancellation_->isCancellationRequested()) {
            result.status = AacAdtsScanStatus::Cancelled;
            return result;
        }
        if (cursor_ >= source_->sizeBytes()) {
            finished_ = true;
            break;
        }

        const auto header = parseHeaderAt(cursor_, &errorMessage);
        if (!errorMessage.isEmpty()) {
            failed_ = true;
            result.status = AacAdtsScanStatus::SourceError;
            result.errorMessage = errorMessage;
            return result;
        }

        const bool headerValid = header.has_value();
        bool emitFrame = false;
        if (inSync_) {
            if (headerValid) {
                emitFrame = true;
            } else {
                inSync_ = false;
            }
        } else if (headerValid && verifyLookahead(cursor_, *header, &errorMessage)) {
            emitFrame = true;
            inSync_ = true;
        }

        if (emitFrame) {
            const auto record = makeRecord(cursor_, *header, &errorMessage);
            if (!record) {
                failed_ = true;
                result.status = AacAdtsScanStatus::SourceError;
                result.errorMessage = errorMessage;
                return result;
            }
            result.records.push_back(*record);
            inspectedBytes_ += header->headerLength;
            ++inspectedPositions;
            if (record->truncated || cursor_ + header->frameLength >= source_->sizeBytes()) {
                cursor_ = source_->sizeBytes();
                finished_ = true;
                break;
            }
            cursor_ += header->frameLength;
        } else {
            // Resynchronization: advance byte-by-byte searching for valid syncword
            ++cursor_;
            ++inspectedBytes_;
            ++inspectedPositions;
        }
    }

    result.status = finished_ ? AacAdtsScanStatus::Complete : AacAdtsScanStatus::InProgress;
    return result;
}

} // namespace streamview::rules
