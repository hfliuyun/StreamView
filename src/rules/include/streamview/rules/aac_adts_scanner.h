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

enum class AacAdtsScanStatus : quint8 {
    InProgress,
    Complete,
    Cancelled,
    SourceError,
    InvalidBatchSize,
};

struct AacAdtsRecord final {
    std::optional<core::SourceSpan> frameSpan;
    std::optional<core::SourceSpan> headerSpan;
    std::optional<core::SourceSpan> payloadSpan;
    quint64 frameOffset = 0;
    quint64 headerLength = 0;
    quint64 payloadLength = 0;
    quint16 aacFrameLength = 0;
    bool crcPresent = false;
    bool truncated = false;
};

struct AacAdtsScanBatch final {
    AacAdtsScanStatus status = AacAdtsScanStatus::Complete;
    std::vector<AacAdtsRecord> records;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept {
        return status == AacAdtsScanStatus::Complete;
    }
};

class AacAdtsScanner final {
public:
    [[nodiscard]] static constexpr quint64 defaultWorkBudget() noexcept {
        return 64U * 1024U;
    }

    explicit AacAdtsScanner(const core::RandomAccessSource& source,
                            std::optional<core::CancellationToken> cancellation = std::nullopt)
        : source_(&source), cancellation_(std::move(cancellation)) {}

    [[nodiscard]] AacAdtsScanBatch scanBatch(
        std::size_t maximumRecords = 256,
        quint64 maximumInspectedPositions = defaultWorkBudget());

    void replaceCancellationToken(
        std::optional<core::CancellationToken> cancellation) noexcept {
        cancellation_ = std::move(cancellation);
    }

    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] quint64 cursor() const noexcept { return cursor_; }

private:
    struct ParsedHeader final {
        quint16 frameLength = 0;
        quint8 headerLength = 0;
        bool crcPresent = false;
    };

    enum class ReadByteStatus : quint8 {
        Available,
        End,
        Error,
    };

    [[nodiscard]] ReadByteStatus readByte(quint64 offset,
                                          quint8* value,
                                          QString* errorMessage);
    [[nodiscard]] std::optional<ParsedHeader> parseHeaderAt(quint64 offset,
                                                            QString* errorMessage);
    [[nodiscard]] bool verifyLookahead(quint64 offset,
                                       const ParsedHeader& header,
                                       QString* errorMessage);
    [[nodiscard]] std::optional<AacAdtsRecord> makeRecord(quint64 offset,
                                                          const ParsedHeader& header,
                                                          QString* errorMessage);

    const core::RandomAccessSource* source_ = nullptr;
    std::optional<core::CancellationToken> cancellation_;
    std::vector<std::byte> buffer_;
    quint64 bufferStart_ = 0;
    quint64 bufferEnd_ = 0;
    quint64 cursor_ = 0;
    quint64 inspectedBytes_ = 0;
    bool inSync_ = true;
    bool finished_ = false;
    bool failed_ = false;
};

} // namespace streamview::rules
