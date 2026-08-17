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

enum class Mp4BoxScanStatus : quint8 {
    InProgress,
    Complete,
    Cancelled,
    SourceError,
    InvalidBatchSize,
};

struct Mp4BoxRecord final {
    std::optional<core::SourceSpan> boxSpan;
    quint64 boxOffset = 0;
    quint64 declaredBoxSize = 0;
    bool truncated = false;
    bool terminal = false;
};

struct Mp4BoxScanBatch final {
    Mp4BoxScanStatus status = Mp4BoxScanStatus::Complete;
    std::vector<Mp4BoxRecord> records;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept {
        return status == Mp4BoxScanStatus::Complete;
    }
};

class Mp4BoxScanner final {
public:
    [[nodiscard]] static constexpr quint64 defaultWorkBudget() noexcept {
        return 64U * 1024U;
    }

    explicit Mp4BoxScanner(const core::RandomAccessSource& source,
                           std::optional<core::CancellationToken> cancellation = std::nullopt)
        : source_(&source), cancellation_(std::move(cancellation)) {}

    [[nodiscard]] Mp4BoxScanBatch scanBatch(
        std::size_t maximumRecords = 256,
        quint64 maximumInspectedPositions = defaultWorkBudget());

    void replaceCancellationToken(
        std::optional<core::CancellationToken> cancellation) noexcept {
        cancellation_ = std::move(cancellation);
    }

    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] quint64 cursor() const noexcept { return cursor_; }

private:
    enum class ReadBytesStatus : quint8 {
        Available,
        End,
        Error,
    };

    [[nodiscard]] ReadBytesStatus readBytes(quint64 offset,
                                            std::size_t count,
                                            std::byte* destination,
                                            QString* errorMessage);

    const core::RandomAccessSource* source_ = nullptr;
    std::optional<core::CancellationToken> cancellation_;
    std::vector<std::byte> buffer_;
    quint64 bufferStart_ = 0;
    quint64 bufferEnd_ = 0;
    quint64 cursor_ = 0;
    quint64 inspectedPositions_ = 0;
    bool finished_ = false;
};

} // namespace streamview::rules
