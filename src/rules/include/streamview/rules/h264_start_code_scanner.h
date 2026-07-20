#pragma once

#include <streamview/core/cancellation.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace streamview::rules {

enum class StartCodeScanStatus : quint8 {
    InProgress,
    Complete,
    Cancelled,
    SourceError,
    InvalidBatchSize,
};

struct H264StartCodeRecord final {
    std::optional<core::SourceSpan> startCode;
    std::optional<core::SourceSpan> nalUnit;
    quint64 startCodeOffset = 0;
    quint8 startCodeLength = 0;
    quint64 nalUnitOffset = 0;
    quint64 nalUnitLength = 0;
};

struct StartCodeScanBatch final {
    StartCodeScanStatus status = StartCodeScanStatus::Complete;
    std::vector<H264StartCodeRecord> records;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept {
        return status == StartCodeScanStatus::Complete;
    }
};

class H264StartCodeScanner final {
public:
    explicit H264StartCodeScanner(const core::RandomAccessSource& source,
                                  std::optional<core::CancellationToken> cancellation = std::nullopt)
        : source_(&source), cancellation_(std::move(cancellation)) {}

    [[nodiscard]] StartCodeScanBatch scanBatch(std::size_t maximumRecords = 256);

    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] quint64 cursor() const noexcept { return cursor_; }

private:
    struct PendingStartCode final {
        quint64 offset = 0;
        quint8 length = 0;
    };

    enum class ReadByteStatus : quint8 {
        Available,
        End,
        Error,
    };

    [[nodiscard]] ReadByteStatus readByte(quint64 offset,
                                          quint8* value,
                                          QString* errorMessage);
    [[nodiscard]] quint8 prefixLengthAt(quint64 offset, QString* errorMessage);
    [[nodiscard]] std::optional<H264StartCodeRecord>
    makeRecord(PendingStartCode start, quint64 endOffset, QString* errorMessage) const;

    const core::RandomAccessSource* source_ = nullptr;
    std::optional<core::CancellationToken> cancellation_;
    std::vector<std::byte> buffer_;
    quint64 bufferStart_ = 0;
    quint64 bufferEnd_ = 0;
    quint64 cursor_ = 0;
    quint64 inspectedBytes_ = 0;
    std::optional<PendingStartCode> pending_;
    bool finished_ = false;
    bool failed_ = false;
};

} // namespace streamview::rules
