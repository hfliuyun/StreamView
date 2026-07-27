#include <streamview/rules/h264_ebsp_rbsp_mapper.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>

namespace streamview::rules {

namespace {

constexpr quint64 kChunkSize = 64U * 1024U;
constexpr quint64 kCancellationInterval = 1024U;

[[nodiscard]] std::optional<core::SourceSpan> makeByteSpan(quint64 byteOffset,
                                                            quint64 byteLength) {
    const auto start = core::SourceBitAddress::fromByteAndBit(byteOffset, 0);
    if (!start ||
        byteLength >
            (std::numeric_limits<quint64>::max() - start->absoluteBitOffset()) / 8U) {
        return std::nullopt;
    }
    return core::SourceSpan::create(*start, byteLength * 8U);
}

} // namespace

H264EbspRbspMapper::H264EbspRbspMapper(
    const core::RandomAccessSource& source,
    core::LogicalViewId rbspViewId,
    core::SourceSpan ebspSpan,
    H264EbspRbspMapLimits limits,
    std::optional<core::CancellationToken> cancellation)
    : source_(&source), rbspViewId_(rbspViewId), ebspSpan_(ebspSpan), limits_(limits),
      cancellation_(std::move(cancellation)), sourceCursor_(ebspSpan.start().byteOffset()),
      mapping_(core::SourceMapping::create(rbspViewId, {})) {
    const auto rejectInput = [this](QString message) {
        terminal_ = true;
        terminalStatus_ = H264EbspRbspMapStatus::InvalidInput;
        terminalErrorMessage_ = std::move(message);
    };

    if (limits_.maximumMappingSegments == 0 || limits_.maximumExcludedSpans == 0 ||
        limits_.maximumIssues == 0) {
        rejectInput(QStringLiteral(
            "H.264 EBSP-to-RBSP cumulative limits must be greater than zero"));
        return;
    }
    if (ebspSpan_.start().bitOffsetInByte() != 0 || ebspSpan_.bitLength() % 8U != 0) {
        rejectInput(QStringLiteral("H.264 EBSP input must be byte-aligned and byte-sized"));
        return;
    }

    const quint64 inputBytes = ebspSpan_.bitLength() / 8U;
    if (sourceCursor_ > source_->sizeBytes() ||
        inputBytes > source_->sizeBytes() - sourceCursor_) {
        rejectInput(QStringLiteral("H.264 EBSP input span exceeds the source"));
        return;
    }
    inputEndByte_ = sourceCursor_ + inputBytes;
    bufferStart_ = sourceCursor_;
    bufferEnd_ = sourceCursor_;
}

H264EbspRbspMapBatch H264EbspRbspMapper::mapBatch(quint64 maximumInspectedBytes) {
    H264EbspRbspMapBatch result;
    if (terminal_) {
        result.status = terminalStatus_;
        result.errorMessage = terminalErrorMessage_;
        return result;
    }
    if (maximumInspectedBytes == 0) {
        result.status = H264EbspRbspMapStatus::InvalidBatchSize;
        result.errorMessage =
            QStringLiteral("Maximum inspected H.264 EBSP bytes must be greater than zero");
        return result;
    }

    const auto refreshMapping = [this]() {
        auto mapping = core::SourceMapping::create(rbspViewId_, forwardedSpans_);
        if (!mapping) {
            return false;
        }
        mapping_ = std::move(*mapping);
        return true;
    };
    const auto finish = [this, &refreshMapping](H264EbspRbspMapStatus status,
                                                QString errorMessage,
                                                bool terminal) {
        if (!refreshMapping()) {
            status = H264EbspRbspMapStatus::ResourceLimit;
            errorMessage = QStringLiteral("Unable to represent the H.264 RBSP source mapping");
            terminal = true;
        }
        if (terminal) {
            terminal_ = true;
            terminalStatus_ = status;
            terminalErrorMessage_ = errorMessage;
        }
        H264EbspRbspMapBatch batch;
        batch.status = status;
        batch.errorMessage = std::move(errorMessage);
        return batch;
    };

    if (cancellation_ && cancellation_->isCancellationRequested()) {
        return finish(H264EbspRbspMapStatus::Cancelled, {}, true);
    }
    if (sourceCursor_ >= inputEndByte_) {
        return finish(H264EbspRbspMapStatus::Complete, {}, true);
    }

    const auto readByte = [this](quint8* value, QString* errorMessage) {
        if (sourceCursor_ < bufferStart_ || sourceCursor_ >= bufferEnd_) {
            bufferStart_ = sourceCursor_;
            const quint64 requested = std::min(kChunkSize, inputEndByte_ - sourceCursor_);
            buffer_.resize(static_cast<std::size_t>(requested));
            const core::SourceReadResult readResult = source_->readAt(
                bufferStart_, std::span<std::byte>(buffer_.data(), buffer_.size()));
            if (!readResult.complete() || readResult.bytesRead != buffer_.size()) {
                if (readResult.complete()) {
                    *errorMessage =
                        QStringLiteral("Source reported an incomplete successful read");
                } else if (!readResult.errorMessage.isEmpty()) {
                    *errorMessage = readResult.errorMessage;
                } else if (readResult.status == core::SourceReadStatus::EndOfSource) {
                    *errorMessage =
                        QStringLiteral("Source ended before the validated H.264 EBSP span");
                } else {
                    *errorMessage =
                        QStringLiteral("Unable to read source while mapping H.264 EBSP");
                }
                return false;
            }
            bufferEnd_ = bufferStart_ + requested;
        }

        const auto index = static_cast<std::size_t>(sourceCursor_ - bufferStart_);
        *value = std::to_integer<quint8>(buffer_.at(index));
        return true;
    };

    quint64 batchInspectedBytes = 0;
    while (sourceCursor_ < inputEndByte_ && batchInspectedBytes < maximumInspectedBytes) {
        quint8 value = 0;
        QString errorMessage;
        if (!readByte(&value, &errorMessage)) {
            return finish(H264EbspRbspMapStatus::SourceError, errorMessage, true);
        }

        const quint64 currentByte = sourceCursor_;
        const auto currentSourceSpan = makeByteSpan(currentByte, 1);
        if (!currentSourceSpan) {
            return finish(H264EbspRbspMapStatus::ResourceLimit,
                          QStringLiteral("H.264 EBSP source position exceeds coordinate limits"),
                          true);
        }
        const bool excludesByte = zeroRun_ >= 2U && value == 0x03U;
        const bool forwardsByte = !excludesByte;

        std::optional<H264EbspRbspIssue> firstIssue;
        std::optional<H264EbspRbspIssue> secondIssue;
        const auto planIssue = [&firstIssue, &secondIssue](H264EbspRbspIssue issue) {
            if (!firstIssue) {
                firstIssue = std::move(issue);
            } else {
                secondIssue = std::move(issue);
            }
        };

        if (pendingEscapeStartByte_ && value > 0x03U) {
            const auto issueSpan = makeByteSpan(*pendingEscapeStartByte_, 4);
            if (!issueSpan) {
                return finish(H264EbspRbspMapStatus::ResourceLimit,
                              QStringLiteral("H.264 EBSP issue exceeds coordinate limits"),
                              true);
            }
            planIssue({H264EbspRbspIssueKind::Prohibited000003xx, *issueSpan});
        }
        if (zeroRun_ >= 2U && value <= 0x02U) {
            const auto issueSpan = makeByteSpan(currentByte - 2U, 3);
            if (!issueSpan) {
                return finish(H264EbspRbspMapStatus::ResourceLimit,
                              QStringLiteral("H.264 EBSP issue exceeds coordinate limits"),
                              true);
            }
            H264EbspRbspIssueKind kind = H264EbspRbspIssueKind::Prohibited000000;
            if (value == 0x01U) {
                kind = H264EbspRbspIssueKind::Prohibited000001;
            } else if (value == 0x02U) {
                kind = H264EbspRbspIssueKind::Prohibited000002;
            }
            planIssue({kind, *issueSpan});
        }
        if (currentByte + 1U == inputEndByte_ && value == 0x00U) {
            planIssue({H264EbspRbspIssueKind::FinalZeroByte, *currentSourceSpan});
        }

        const std::size_t plannedIssueCount = static_cast<std::size_t>(firstIssue.has_value()) +
                                              static_cast<std::size_t>(secondIssue.has_value());
        if (issues_.size() > limits_.maximumIssues ||
            plannedIssueCount > limits_.maximumIssues - issues_.size()) {
            return finish(H264EbspRbspMapStatus::ResourceLimit,
                          QStringLiteral("H.264 EBSP conformance issue limit exceeded"),
                          true);
        }
        if (excludesByte && excludedSpans_.size() >= limits_.maximumExcludedSpans) {
            return finish(H264EbspRbspMapStatus::ResourceLimit,
                          QStringLiteral("H.264 EBSP excluded-span limit exceeded"),
                          true);
        }

        const quint64 currentBit = currentSourceSpan->start().absoluteBitOffset();
        const bool startsMappingSegment =
            forwardsByte &&
            (forwardedSpans_.empty() ||
             forwardedSpans_.back().endExclusive().absoluteBitOffset() != currentBit);
        if (startsMappingSegment &&
            forwardedSpans_.size() >= limits_.maximumMappingSegments) {
            return finish(H264EbspRbspMapStatus::ResourceLimit,
                          QStringLiteral("H.264 RBSP mapping segment limit exceeded"),
                          true);
        }
        if (forwardsByte &&
            rbspLogicalBitLength_ > std::numeric_limits<quint64>::max() - 8U) {
            return finish(H264EbspRbspMapStatus::ResourceLimit,
                          QStringLiteral("H.264 RBSP logical length exceeds coordinate limits"),
                          true);
        }
        std::optional<core::SourceSpan> mergedForwardedSpan;
        if (forwardsByte && !startsMappingSegment) {
            const core::SourceSpan& previous = forwardedSpans_.back();
            if (previous.bitLength() > std::numeric_limits<quint64>::max() - 8U) {
                return finish(H264EbspRbspMapStatus::ResourceLimit,
                              QStringLiteral("H.264 RBSP source span exceeds coordinate limits"),
                              true);
            }
            mergedForwardedSpan =
                core::SourceSpan::create(previous.start(), previous.bitLength() + 8U);
            if (!mergedForwardedSpan) {
                return finish(H264EbspRbspMapStatus::ResourceLimit,
                              QStringLiteral("H.264 RBSP source span exceeds coordinate limits"),
                              true);
            }
        }

        if (firstIssue) {
            issues_.push_back(*firstIssue);
        }
        if (secondIssue) {
            issues_.push_back(*secondIssue);
        }

        if (excludesByte) {
            excludedSpans_.push_back({*currentSourceSpan, rbspLogicalBitLength_});
        } else {
            if (startsMappingSegment) {
                forwardedSpans_.push_back(*currentSourceSpan);
            } else {
                forwardedSpans_.back() = *mergedForwardedSpan;
            }
            rbspLogicalBitLength_ += 8U;
        }

        pendingEscapeStartByte_ =
            excludesByte ? std::optional<quint64>(currentByte - 2U) : std::nullopt;
        if (excludesByte) {
            zeroRun_ = 0;
        } else if (value == 0x00U) {
            zeroRun_ = static_cast<quint8>(std::min<unsigned int>(2U, zeroRun_ + 1U));
        } else {
            zeroRun_ = 0;
        }

        ++sourceCursor_;
        ++inspectedBytes_;
        ++batchInspectedBytes;

        if (cancellation_ && inspectedBytes_ % kCancellationInterval == 0 &&
            cancellation_->isCancellationRequested()) {
            return finish(H264EbspRbspMapStatus::Cancelled, {}, true);
        }
    }

    if (sourceCursor_ >= inputEndByte_) {
        return finish(H264EbspRbspMapStatus::Complete, {}, true);
    }
    return finish(H264EbspRbspMapStatus::InProgress, {}, false);
}

} // namespace streamview::rules
