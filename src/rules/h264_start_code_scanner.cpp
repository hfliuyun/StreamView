#include <streamview/rules/h264_start_code_scanner.h>

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

H264StartCodeScanner::ReadByteStatus H264StartCodeScanner::readByte(quint64 offset,
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
                                ? QStringLiteral("Unable to read source while scanning start codes")
                                : readResult.errorMessage;
            return ReadByteStatus::Error;
        }
        bufferEnd_ = bufferStart_ + requested;
    }
    const auto index = static_cast<std::size_t>(offset - bufferStart_);
    *value = std::to_integer<quint8>(buffer_.at(index));
    return ReadByteStatus::Available;
}

quint8 H264StartCodeScanner::prefixLengthAt(quint64 offset, QString* errorMessage) {
    quint8 first = 0;
    quint8 second = 0;
    quint8 third = 0;
    quint8 fourth = 0;
    if (readByte(offset, &first, errorMessage) == ReadByteStatus::Error || first != 0) {
        return 0;
    }
    if (readByte(offset + 1U, &second, errorMessage) == ReadByteStatus::Error || second != 0) {
        return 0;
    }
    if (readByte(offset + 2U, &third, errorMessage) == ReadByteStatus::Error) {
        return 0;
    }
    if (third == 1) {
        return 3;
    }
    if (third != 0 || readByte(offset + 3U, &fourth, errorMessage) == ReadByteStatus::Error) {
        return 0;
    }
    return fourth == 1 ? 4 : 0;
}

std::optional<H264StartCodeRecord>
H264StartCodeScanner::makeRecord(PendingStartCode start,
                                 quint64 endOffset,
                                 QString* errorMessage) const {
    if (start.offset > std::numeric_limits<quint64>::max() - start.length ||
        endOffset < start.offset + start.length) {
        *errorMessage = QStringLiteral("Start-code offsets are not ordered");
        return std::nullopt;
    }
    const quint64 nalOffset = start.offset + start.length;
    const quint64 nalLength = endOffset - nalOffset;
    H264StartCodeRecord record;
    record.startCodeOffset = start.offset;
    record.startCodeLength = start.length;
    record.nalUnitOffset = nalOffset;
    record.nalUnitLength = nalLength;
    record.startCode = makeByteSpan(start.offset, start.length);
    if (nalLength != 0) {
        record.nalUnit = makeByteSpan(nalOffset, nalLength);
    }
    if (!record.startCode || (nalLength != 0 && !record.nalUnit)) {
        *errorMessage = QStringLiteral("Start-code span exceeds bit coordinate limits");
        return std::nullopt;
    }
    return record;
}

StartCodeScanBatch H264StartCodeScanner::scanBatch(std::size_t maximumRecords) {
    StartCodeScanBatch result;
    if (maximumRecords == 0) {
        result.status = StartCodeScanStatus::InvalidBatchSize;
        result.errorMessage = QStringLiteral("Maximum scan records must be greater than zero");
        return result;
    }
    if (source_->sizeBytes() > std::numeric_limits<quint64>::max() / 8U) {
        failed_ = true;
        result.status = StartCodeScanStatus::SourceError;
        result.errorMessage =
            QStringLiteral("Source size exceeds the bit coordinate representation limit");
        return result;
    }
    if (failed_) {
        result.status = StartCodeScanStatus::SourceError;
        result.errorMessage = QStringLiteral("Start-code scanner is in a failed state");
        return result;
    }
    if (finished_) {
        result.status = StartCodeScanStatus::Complete;
        return result;
    }

    QString errorMessage;
    while (!finished_ && result.records.size() < maximumRecords) {
        if (cancellation_ && (inspectedBytes_ % 1024U == 0) &&
            cancellation_->isCancellationRequested()) {
            result.status = StartCodeScanStatus::Cancelled;
            return result;
        }
        if (cursor_ >= source_->sizeBytes()) {
            finished_ = true;
            if (pending_) {
                const auto record = makeRecord(*pending_, source_->sizeBytes(), &errorMessage);
                if (!record) {
                    failed_ = true;
                    result.status = StartCodeScanStatus::SourceError;
                    result.errorMessage = errorMessage;
                    return result;
                }
                result.records.push_back(*record);
                pending_.reset();
            }
            break;
        }

        const quint8 prefixLength = prefixLengthAt(cursor_, &errorMessage);
        if (!errorMessage.isEmpty()) {
            failed_ = true;
            result.status = StartCodeScanStatus::SourceError;
            result.errorMessage = errorMessage;
            return result;
        }
        ++inspectedBytes_;
        if (prefixLength == 0) {
            ++cursor_;
            continue;
        }

        const PendingStartCode current{cursor_, prefixLength};
        if (pending_) {
            const auto record = makeRecord(*pending_, current.offset, &errorMessage);
            if (!record) {
                failed_ = true;
                result.status = StartCodeScanStatus::SourceError;
                result.errorMessage = errorMessage;
                return result;
            }
            result.records.push_back(*record);
        }
        pending_ = current;
        cursor_ += prefixLength;
    }

    result.status = finished_ ? StartCodeScanStatus::Complete : StartCodeScanStatus::InProgress;
    return result;
}

} // namespace streamview::rules
