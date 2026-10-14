#include <streamview/rules/riff_chunk_scanner.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace streamview::rules {

namespace {

constexpr quint64 kChunkSize = 64U * 1024U;

constexpr quint32 kRiffMagic = 0x52494646U;   // 'RIFF'
constexpr quint32 kWaveForm = 0x57415645U;    // 'WAVE'

[[nodiscard]] quint32 readBigEndianU32(const std::byte* bytes) noexcept {
    return (static_cast<quint32>(static_cast<quint8>(bytes[0])) << 24) |
           (static_cast<quint32>(static_cast<quint8>(bytes[1])) << 16) |
           (static_cast<quint32>(static_cast<quint8>(bytes[2])) << 8) |
           static_cast<quint32>(static_cast<quint8>(bytes[3]));
}

[[nodiscard]] quint32 readLittleEndianU32(const std::byte* bytes) noexcept {
    return (static_cast<quint32>(static_cast<quint8>(bytes[3])) << 24) |
           (static_cast<quint32>(static_cast<quint8>(bytes[2])) << 16) |
           (static_cast<quint32>(static_cast<quint8>(bytes[1])) << 8) |
           static_cast<quint32>(static_cast<quint8>(bytes[0]));
}

[[nodiscard]] bool checkBitCoordinateOverflow(quint64 offsetBytes, quint64 lengthBytes) noexcept {
    constexpr quint64 maxBytes = std::numeric_limits<quint64>::max() / 8U;
    if (offsetBytes > maxBytes || lengthBytes > maxBytes) {
        return false;
    }
    return lengthBytes <= (maxBytes - offsetBytes);
}

} // namespace

RiffChunkScanner::ReadBytesStatus RiffChunkScanner::readBytes(quint64 offset,
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

    if (offset >= bufferStart_ && offset < bufferEnd_ && count <= (bufferEnd_ - offset)) {
        const std::size_t bufferOffset = static_cast<std::size_t>(offset - bufferStart_);
        std::memcpy(destination, buffer_.data() + bufferOffset, count);
        return ReadBytesStatus::Available;
    }

    bufferStart_ = offset;
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
                                ? QStringLiteral("Unable to read source while scanning RIFF chunks")
                                : readResult.errorMessage;
        }
        return ReadBytesStatus::Error;
    }
    bufferEnd_ = bufferStart_ + requested;

    if (count > (bufferEnd_ - offset)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Read count exceeds buffer range");
        }
        return ReadBytesStatus::Error;
    }

    const std::size_t bufferOffset = static_cast<std::size_t>(offset - bufferStart_);
    std::memcpy(destination, buffer_.data() + bufferOffset, count);
    return ReadBytesStatus::Available;
}

RiffChunkScanBatch RiffChunkScanner::scanBatch(std::size_t maximumRecords,
                                               quint64 maximumInspectedPositions) {
    RiffChunkScanBatch batch;

    if (maximumRecords == 0 || maximumInspectedPositions == 0) {
        batch.status = RiffChunkScanStatus::InvalidBatchSize;
        batch.errorMessage = QStringLiteral("Batch maximum record count and position limit must be greater than zero");
        return batch;
    }

    if (failed_) {
        batch.status = RiffChunkScanStatus::SourceError;
        batch.errorMessage = lastErrorMessage_;
        return batch;
    }

    if (source_ == nullptr) {
        batch.status = RiffChunkScanStatus::SourceError;
        batch.errorMessage = QStringLiteral("Source is not available");
        failed_ = true;
        lastErrorMessage_ = batch.errorMessage;
        return batch;
    }

    const quint64 sourceSize = source_->sizeBytes();
    if (finished_ || cursor_ >= sourceSize) {
        finished_ = true;
        batch.status = RiffChunkScanStatus::Complete;
        return batch;
    }

    if (cancellation_.has_value() && cancellation_->isCancellationRequested()) {
        batch.status = RiffChunkScanStatus::Cancelled;
        return batch;
    }

    batch.status = RiffChunkScanStatus::InProgress;
    inspectedPositions_ = 0;

    // Frame the 12-byte RIFF form header exactly once. A source that cannot
    // provide the header, or that does not carry the RIFF/WAVE magics, yields
    // zero records: the detector owns candidate gating, and the analyzer
    // surfaces the diagnostic for a routed-but-unframable source.
    if (!headerScanned_) {
        headerScanned_ = true;

        if (sourceSize < 12U) {
            finished_ = true;
            batch.status = RiffChunkScanStatus::Complete;
            return batch;
        }

        std::byte headerBytes[12];
        const auto headerStatus = readBytes(0U, 12U, headerBytes, &batch.errorMessage);
        if (headerStatus == ReadBytesStatus::Error) {
            batch.status = RiffChunkScanStatus::SourceError;
            failed_ = true;
            lastErrorMessage_ = batch.errorMessage;
            return batch;
        }
        if (headerStatus == ReadBytesStatus::End) {
            finished_ = true;
            batch.status = RiffChunkScanStatus::Complete;
            return batch;
        }

        const quint32 riffMagic = readBigEndianU32(headerBytes);
        const quint32 formType = readBigEndianU32(headerBytes + 8U);
        if (riffMagic != kRiffMagic || formType != kWaveForm) {
            finished_ = true;
            batch.status = RiffChunkScanStatus::Complete;
            return batch;
        }

        const quint64 riffSize = static_cast<quint64>(readLittleEndianU32(headerBytes + 4U));
        if (!checkBitCoordinateOverflow(0U, 12U)) {
            batch.status = RiffChunkScanStatus::SourceError;
            batch.errorMessage = QStringLiteral("Coordinate arithmetic overflow");
            failed_ = true;
            lastErrorMessage_ = batch.errorMessage;
            return batch;
        }
        const auto headerSpan = core::SourceSpan::create(
            core::SourceBitAddress(0U), 12U * 8U);
        if (!headerSpan.has_value()) {
            batch.status = RiffChunkScanStatus::SourceError;
            batch.errorMessage = QStringLiteral("Failed to create source span");
            failed_ = true;
            lastErrorMessage_ = batch.errorMessage;
            return batch;
        }

        RiffChunkRecord headerRecord;
        headerRecord.chunkSpan = headerSpan;
        headerRecord.chunkOffset = 0U;
        headerRecord.chunkId = riffMagic;
        headerRecord.declaredSize = riffSize;
        headerRecord.isFormHeader = true;
        headerRecord.truncated = false;
        headerRecord.terminal = false;
        batch.records.push_back(headerRecord);

        // A degenerate form size cannot frame any chunk; the header record
        // alone is the whole walk.
        if (riffSize < 4U) {
            cursor_ = sourceSize;
            finished_ = true;
            batch.status = RiffChunkScanStatus::Complete;
            return batch;
        }

        formEnd_ = std::min(8U + riffSize, sourceSize);
        cursor_ = 12U;
        if (cursor_ >= formEnd_) {
            finished_ = true;
            batch.status = RiffChunkScanStatus::Complete;
            return batch;
        }
    }

    while (batch.records.size() < maximumRecords &&
           inspectedPositions_ < maximumInspectedPositions &&
           cursor_ < formEnd_) {
        if (cancellation_.has_value() && cancellation_->isCancellationRequested()) {
            batch.status = RiffChunkScanStatus::Cancelled;
            return batch;
        }

        const quint64 start = cursor_;
        const quint64 remaining = formEnd_ - start;

        if (remaining < 8U) {
            // Trailing bytes too small to frame a chunk header stay unclaimed.
            finished_ = true;
            batch.status = RiffChunkScanStatus::Complete;
            return batch;
        }

        ++inspectedPositions_;

        std::byte headerBytes[8];
        const auto headerStatus = readBytes(start, 8U, headerBytes, &batch.errorMessage);
        if (headerStatus == ReadBytesStatus::Error) {
            batch.status = RiffChunkScanStatus::SourceError;
            failed_ = true;
            lastErrorMessage_ = batch.errorMessage;
            return batch;
        }
        if (headerStatus == ReadBytesStatus::End) {
            finished_ = true;
            batch.status = RiffChunkScanStatus::Complete;
            return batch;
        }

        const quint32 chunkId = readBigEndianU32(headerBytes);
        const quint64 chunkSize = static_cast<quint64>(readLittleEndianU32(headerBytes + 4U));

        // 8 (header) + size (<= 0xFFFFFFFF) + 1 (pad) cannot overflow quint64.
        const quint64 chunkTotal = 8U + chunkSize + (chunkSize & 1U);

        if (chunkTotal <= remaining) {
            if (!checkBitCoordinateOverflow(start, chunkTotal)) {
                batch.status = RiffChunkScanStatus::SourceError;
                batch.errorMessage = QStringLiteral("Coordinate arithmetic overflow");
                failed_ = true;
                lastErrorMessage_ = batch.errorMessage;
                return batch;
            }

            const auto span = core::SourceSpan::create(
                core::SourceBitAddress(start * 8U), chunkTotal * 8U);
            if (!span.has_value()) {
                batch.status = RiffChunkScanStatus::SourceError;
                batch.errorMessage = QStringLiteral("Failed to create source span");
                failed_ = true;
                lastErrorMessage_ = batch.errorMessage;
                return batch;
            }

            RiffChunkRecord record;
            record.chunkSpan = span;
            record.chunkOffset = start;
            record.chunkId = chunkId;
            record.declaredSize = chunkSize;
            record.truncated = false;
            record.terminal = false;
            batch.records.push_back(record);

            cursor_ = start + chunkTotal;
        } else {
            // The chunk payload runs past the form/source bound: clamp the
            // record to the remaining bytes and finish the walk.
            if (!checkBitCoordinateOverflow(start, remaining)) {
                batch.status = RiffChunkScanStatus::SourceError;
                batch.errorMessage = QStringLiteral("Coordinate arithmetic overflow");
                failed_ = true;
                lastErrorMessage_ = batch.errorMessage;
                return batch;
            }

            const auto span = core::SourceSpan::create(
                core::SourceBitAddress(start * 8U), remaining * 8U);
            if (!span.has_value()) {
                batch.status = RiffChunkScanStatus::SourceError;
                batch.errorMessage = QStringLiteral("Failed to create source span");
                failed_ = true;
                lastErrorMessage_ = batch.errorMessage;
                return batch;
            }

            RiffChunkRecord record;
            record.chunkSpan = span;
            record.chunkOffset = start;
            record.chunkId = chunkId;
            record.declaredSize = chunkSize;
            record.truncated = true;
            record.terminal = true;
            batch.records.push_back(record);

            cursor_ = formEnd_;
            finished_ = true;
            batch.status = RiffChunkScanStatus::Complete;
            return batch;
        }
    }

    if (cursor_ >= formEnd_) {
        finished_ = true;
        batch.status = RiffChunkScanStatus::Complete;
    } else if (batch.records.size() >= maximumRecords ||
               inspectedPositions_ >= maximumInspectedPositions) {
        batch.status = RiffChunkScanStatus::InProgress;
    }

    return batch;
}

} // namespace streamview::rules
