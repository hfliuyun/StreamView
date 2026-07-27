#pragma once

#include <streamview/core/cancellation.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <vector>

namespace streamview::rules {

enum class H264EbspRbspMapStatus : quint8 {
    InProgress,
    Complete,
    Cancelled,
    SourceError,
    InvalidInput,
    InvalidBatchSize,
    ResourceLimit,
};

enum class H264EbspRbspIssueKind : quint8 {
    Prohibited000000,
    Prohibited000001,
    Prohibited000002,
    Prohibited000003xx,
    FinalZeroByte,
};

struct H264EbspRbspExcludedSpan final {
    core::SourceSpan sourceSpan;
    quint64 rbspBitOffset = 0;
};

struct H264EbspRbspIssue final {
    H264EbspRbspIssueKind kind = H264EbspRbspIssueKind::Prohibited000000;
    core::SourceSpan sourceSpan;
};

struct H264EbspRbspMapLimits final {
    std::size_t maximumMappingSegments = 65'536;
    std::size_t maximumExcludedSpans = 65'536;
    std::size_t maximumIssues = 1'024;
};

struct H264EbspRbspMapBatch final {
    H264EbspRbspMapStatus status = H264EbspRbspMapStatus::InProgress;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept {
        return status == H264EbspRbspMapStatus::Complete;
    }
};

class H264EbspRbspMapper final {
public:
    [[nodiscard]] static constexpr quint64 defaultWorkBudget() noexcept {
        return 64U * 1024U;
    }

    explicit H264EbspRbspMapper(
        const core::RandomAccessSource& source,
        core::LogicalViewId rbspViewId,
        core::SourceSpan ebspSpan,
        H264EbspRbspMapLimits limits = {},
        std::optional<core::CancellationToken> cancellation = std::nullopt);

    H264EbspRbspMapper(const H264EbspRbspMapper&) = delete;
    H264EbspRbspMapper(H264EbspRbspMapper&&) noexcept = default;
    H264EbspRbspMapper& operator=(const H264EbspRbspMapper&) = delete;
    H264EbspRbspMapper& operator=(H264EbspRbspMapper&&) noexcept = default;

    [[nodiscard]] H264EbspRbspMapBatch
    mapBatch(quint64 maximumInspectedBytes = defaultWorkBudget());

    [[nodiscard]] bool finished() const noexcept {
        return terminal_ && terminalStatus_ == H264EbspRbspMapStatus::Complete;
    }
    [[nodiscard]] quint64 sourceCursor() const noexcept { return sourceCursor_; }
    [[nodiscard]] quint64 rbspLogicalBitLength() const noexcept {
        return rbspLogicalBitLength_;
    }
    // A later non-const mapper call may replace the cached mapping.
    [[nodiscard]] const core::SourceMapping& mapping() const { return mapping_.value(); }
    [[nodiscard]] const std::vector<H264EbspRbspExcludedSpan>& excludedSpans() const noexcept {
        return excludedSpans_;
    }
    [[nodiscard]] const std::vector<H264EbspRbspIssue>& issues() const noexcept {
        return issues_;
    }

private:
    const core::RandomAccessSource* source_ = nullptr;
    core::LogicalViewId rbspViewId_;
    core::SourceSpan ebspSpan_;
    H264EbspRbspMapLimits limits_;
    std::optional<core::CancellationToken> cancellation_;

    std::vector<std::byte> buffer_;
    quint64 bufferStart_ = 0;
    quint64 bufferEnd_ = 0;
    quint64 inputEndByte_ = 0;
    quint64 sourceCursor_ = 0;
    quint64 inspectedBytes_ = 0;
    quint64 rbspLogicalBitLength_ = 0;
    quint8 zeroRun_ = 0;
    std::optional<quint64> pendingEscapeStartByte_;

    std::vector<core::SourceSpan> forwardedSpans_;
    std::optional<core::SourceMapping> mapping_;
    std::vector<H264EbspRbspExcludedSpan> excludedSpans_;
    std::vector<H264EbspRbspIssue> issues_;

    bool terminal_ = false;
    H264EbspRbspMapStatus terminalStatus_ = H264EbspRbspMapStatus::InProgress;
    QString terminalErrorMessage_;
};

} // namespace streamview::rules
