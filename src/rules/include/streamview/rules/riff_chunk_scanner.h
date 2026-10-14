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

enum class RiffChunkScanStatus : quint8 {
    InProgress,
    Complete,
    Cancelled,
    SourceError,
    InvalidBatchSize,
};

/// One framing record of a RIFF/WAVE form. The 12-byte form header
/// (`'RIFF'` + form size + `'WAVE'`) is emitted as the first record so a
/// sequence element struct can dispatch on the leading FourCC, per
/// ADR-0115. Chunk records cover the id, size, payload, and pad byte when
/// present.
struct RiffChunkRecord final {
    std::optional<core::SourceSpan> chunkSpan;
    quint64 chunkOffset = 0;
    quint32 chunkId = 0;
    quint64 declaredSize = 0;
    bool isFormHeader = false;
    bool truncated = false;
    bool terminal = false;
};

struct RiffChunkScanBatch final {
    RiffChunkScanStatus status = RiffChunkScanStatus::Complete;
    std::vector<RiffChunkRecord> records;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept {
        return status == RiffChunkScanStatus::Complete;
    }
};

/// Steps the RIFF/WAVE chunk chain with length-chain semantics:
/// next = current + 8 + declaredSize + (declaredSize & 1). The walk is
/// bounded by the form end (8 + riffSize) and the source size. There is no
/// resynchronization: once a chunk cannot be framed the walk finishes,
/// mirroring the ADTS fast-path paradigm recorded in ADR-0115.
class RiffChunkScanner final {
public:
    [[nodiscard]] static constexpr quint64 defaultWorkBudget() noexcept {
        return 64U * 1024U;
    }

    explicit RiffChunkScanner(const core::RandomAccessSource& source,
                              std::optional<core::CancellationToken> cancellation = std::nullopt)
        : source_(&source), cancellation_(std::move(cancellation)) {}

    [[nodiscard]] RiffChunkScanBatch scanBatch(
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
    quint64 formEnd_ = 0;
    bool headerScanned_ = false;
    bool finished_ = false;
    bool failed_ = false;
    QString lastErrorMessage_;
};

} // namespace streamview::rules
