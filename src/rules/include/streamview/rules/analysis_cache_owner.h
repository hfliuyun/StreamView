#pragma once

#include <streamview/core/paged_cache.h>
#include <streamview/rules/analysis_cache.h>
#include <streamview/rules/analysis_cache_payload.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <vector>

namespace streamview::rules {

struct AnalysisCacheOwnerOptions final {
    std::size_t maximumOutstandingRequests = 64;
    std::size_t maximumRetainedWriteBytes =
        core::PagedCache::pageSizeBytes() * core::PagedCache::maximumBatchPages();
};

enum class AnalysisCacheOwnerStartStatus : quint8 {
    Started,
    InvalidArgument,
    WorkerStartFailed,
    CacheOpenFailed,
};

enum class AnalysisCacheOwnerSubmitStatus : quint8 {
    Accepted,
    InvalidArgument,
    UnsupportedValue,
    PayloadTooLarge,
    QueueFull,
    ShuttingDown,
};

enum class AnalysisCacheOwnerWriteStatus : quint8 {
    Committed,
    StorageError,
};

struct AnalysisCacheOwnerWriteResult final {
    AnalysisCacheOwnerWriteStatus status = AnalysisCacheOwnerWriteStatus::StorageError;
    std::optional<core::PagedCacheCommitStatus> cacheStatus;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == AnalysisCacheOwnerWriteStatus::Committed;
    }
};

enum class AnalysisCacheOwnerReadStatus : quint8 {
    Found,
    Missing,
    InvalidRequest,
    Corrupt,
    StorageError,
};

struct H264ProgressiveIndexCacheReadResult final {
    AnalysisCacheOwnerReadStatus status = AnalysisCacheOwnerReadStatus::InvalidRequest;
    std::optional<H264ProgressiveIndexCachePage> page;
    std::optional<core::PagedCacheReadStatus> cacheStatus;
    std::optional<AnalysisCacheEnvelopeDecodeStatus> envelopeStatus;
    std::optional<AnalysisCacheBodyDecodeStatus> bodyStatus;
    QString errorMessage;

    [[nodiscard]] bool found() const noexcept {
        return status == AnalysisCacheOwnerReadStatus::Found && page.has_value();
    }
};

struct MaterializedResultCacheReadResult final {
    AnalysisCacheOwnerReadStatus status = AnalysisCacheOwnerReadStatus::InvalidRequest;
    std::optional<MaterializedResultCachePage> page;
    std::optional<core::PagedCacheReadStatus> cacheStatus;
    std::optional<AnalysisCacheEnvelopeDecodeStatus> envelopeStatus;
    std::optional<AnalysisCacheBodyDecodeStatus> bodyStatus;
    QString errorMessage;

    [[nodiscard]] bool found() const noexcept {
        return status == AnalysisCacheOwnerReadStatus::Found && page.has_value();
    }
};

template <typename Result>
struct AnalysisCacheOwnerSubmission final {
    AnalysisCacheOwnerSubmitStatus status = AnalysisCacheOwnerSubmitStatus::InvalidArgument;
    std::future<Result> completion;
    QString errorMessage;

    [[nodiscard]] bool accepted() const noexcept {
        return status == AnalysisCacheOwnerSubmitStatus::Accepted && completion.valid();
    }
};

using AnalysisCacheOwnerWriteSubmission =
    AnalysisCacheOwnerSubmission<AnalysisCacheOwnerWriteResult>;
using H264ProgressiveIndexCacheReadSubmission =
    AnalysisCacheOwnerSubmission<H264ProgressiveIndexCacheReadResult>;
using MaterializedResultCacheReadSubmission =
    AnalysisCacheOwnerSubmission<MaterializedResultCacheReadResult>;

enum class AnalysisCacheOwnerFlushStatus : quint8 {
    Drained,
    ShutDown,
};

struct AnalysisCacheOwnerStartResult;

class AnalysisCacheOwner final {
public:
    [[nodiscard]] static constexpr std::size_t maximumOutstandingRequests() noexcept {
        return 1024;
    }
    [[nodiscard]] static constexpr std::size_t maximumRetainedWriteBytes() noexcept {
        return core::PagedCache::pageSizeBytes() * core::PagedCache::maximumBatchPages();
    }

    [[nodiscard]] static AnalysisCacheOwnerStartResult
    start(const QString& databasePath,
          AnalysisCacheNamespace cacheNamespace,
          AnalysisCacheOwnerOptions options = {});

    ~AnalysisCacheOwner();
    AnalysisCacheOwner(const AnalysisCacheOwner&) = delete;
    AnalysisCacheOwner& operator=(const AnalysisCacheOwner&) = delete;
    AnalysisCacheOwner(AnalysisCacheOwner&&) = delete;
    AnalysisCacheOwner& operator=(AnalysisCacheOwner&&) = delete;

    [[nodiscard]] AnalysisCacheOwnerWriteSubmission
    writeProgressiveIndex(std::vector<H264ProgressiveIndexCachePage> pages);
    [[nodiscard]] AnalysisCacheOwnerWriteSubmission
    writeMaterializedResult(std::vector<MaterializedResultCachePage> pages);

    [[nodiscard]] H264ProgressiveIndexCacheReadSubmission
    readProgressiveIndex(core::PagedCachePageKey key);
    [[nodiscard]] MaterializedResultCacheReadSubmission
    readMaterializedResult(core::PagedCachePageKey key);

    [[nodiscard]] AnalysisCacheOwnerFlushStatus flush();
    void shutdown();

private:
    class Impl;
    explicit AnalysisCacheOwner(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

struct AnalysisCacheOwnerStartResult final {
    AnalysisCacheOwnerStartStatus status = AnalysisCacheOwnerStartStatus::CacheOpenFailed;
    std::unique_ptr<AnalysisCacheOwner> owner;
    std::optional<core::PagedCacheOpenStatus> cacheStatus;
    quint64 recoveredIncompleteBatchCount = 0;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == AnalysisCacheOwnerStartStatus::Started && owner != nullptr;
    }
};

} // namespace streamview::rules
