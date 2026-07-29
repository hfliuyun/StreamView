#include <streamview/rules/analysis_cache_owner.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace streamview::rules {

namespace {

struct PreparedWriteBatch final {
    AnalysisCacheOwnerSubmitStatus status = AnalysisCacheOwnerSubmitStatus::InvalidArgument;
    std::vector<core::PagedCachePageWrite> writes;
    std::size_t retainedBytes = 0;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == AnalysisCacheOwnerSubmitStatus::Accepted;
    }
};

[[nodiscard]] AnalysisCacheOwnerSubmitStatus
submitStatus(AnalysisCacheBodyEncodeStatus status) noexcept {
    switch (status) {
    case AnalysisCacheBodyEncodeStatus::Encoded:
        return AnalysisCacheOwnerSubmitStatus::Accepted;
    case AnalysisCacheBodyEncodeStatus::InvalidArgument:
        return AnalysisCacheOwnerSubmitStatus::InvalidArgument;
    case AnalysisCacheBodyEncodeStatus::UnsupportedValue:
        return AnalysisCacheOwnerSubmitStatus::UnsupportedValue;
    case AnalysisCacheBodyEncodeStatus::PayloadTooLarge:
        return AnalysisCacheOwnerSubmitStatus::PayloadTooLarge;
    }
    return AnalysisCacheOwnerSubmitStatus::InvalidArgument;
}

[[nodiscard]] AnalysisCacheOwnerSubmitStatus
submitStatus(AnalysisCacheEnvelopeEncodeStatus status) noexcept {
    switch (status) {
    case AnalysisCacheEnvelopeEncodeStatus::Encoded:
        return AnalysisCacheOwnerSubmitStatus::Accepted;
    case AnalysisCacheEnvelopeEncodeStatus::InvalidArgument:
        return AnalysisCacheOwnerSubmitStatus::InvalidArgument;
    case AnalysisCacheEnvelopeEncodeStatus::PayloadTooLarge:
        return AnalysisCacheOwnerSubmitStatus::PayloadTooLarge;
    }
    return AnalysisCacheOwnerSubmitStatus::InvalidArgument;
}

template <typename Page, typename Encoder>
[[nodiscard]] PreparedWriteBatch prepareWriteBatch(
    const AnalysisCacheNamespace& cacheNamespace,
    std::vector<Page> pages,
    core::PagedCachePageKind expectedKind,
    Encoder encoder) {
    if (pages.empty() || pages.size() > core::PagedCache::maximumBatchPages()) {
        return {AnalysisCacheOwnerSubmitStatus::InvalidArgument, {}, 0,
                QStringLiteral("Analysis cache owner batch must contain between 1 and 256 pages")};
    }

    std::set<core::PagedCachePageKey> keys;
    PreparedWriteBatch result;
    result.status = AnalysisCacheOwnerSubmitStatus::Accepted;
    result.writes.reserve(pages.size());
    for (const Page& page : pages) {
        if (page.key.kind != expectedKind || !keys.insert(page.key).second) {
            return {AnalysisCacheOwnerSubmitStatus::InvalidArgument, {}, 0,
                    QStringLiteral("Analysis cache owner batch has an invalid or duplicate key")};
        }

        AnalysisCacheBodyEncodeResult body = encoder(page);
        if (!body.succeeded()) {
            return {submitStatus(body.status), {}, 0, std::move(body.errorMessage)};
        }
        AnalysisCacheEnvelopeEncodeResult envelope =
            AnalysisCachePayloadEnvelope::encode(cacheNamespace, expectedKind, body.bytes);
        if (!envelope.succeeded()) {
            return {submitStatus(envelope.status), {}, 0, std::move(envelope.errorMessage)};
        }
        if (result.retainedBytes >
            std::numeric_limits<std::size_t>::max() - envelope.bytes.size()) {
            return {AnalysisCacheOwnerSubmitStatus::PayloadTooLarge, {}, 0,
                    QStringLiteral("Analysis cache owner batch byte size overflows")};
        }
        result.retainedBytes += envelope.bytes.size();
        result.writes.push_back({page.key, std::move(envelope.bytes)});
    }
    return result;
}

[[nodiscard]] bool validReadKey(const core::PagedCachePageKey& key,
                                core::PagedCachePageKind expectedKind) noexcept {
    constexpr quint64 maximumCoordinate =
        static_cast<quint64>(std::numeric_limits<qlonglong>::max());
    return key.kind == expectedKind && key.streamId <= maximumCoordinate &&
           key.pageIndex <= maximumCoordinate;
}

[[nodiscard]] AnalysisCacheOwnerWriteResult workerFailureWriteResult(const QString& message) {
    AnalysisCacheOwnerWriteResult result;
    result.errorMessage = message;
    return result;
}

[[nodiscard]] H264ProgressiveIndexCacheReadResult
workerFailureProgressiveReadResult(const QString& message) {
    H264ProgressiveIndexCacheReadResult result;
    result.status = AnalysisCacheOwnerReadStatus::StorageError;
    result.errorMessage = message;
    return result;
}

[[nodiscard]] MaterializedResultCacheReadResult
workerFailureMaterializedReadResult(const QString& message) {
    MaterializedResultCacheReadResult result;
    result.status = AnalysisCacheOwnerReadStatus::StorageError;
    result.errorMessage = message;
    return result;
}

} // namespace

class AnalysisCacheOwner::Impl final {
public:
    Impl(QString databasePath,
         AnalysisCacheNamespace cacheNamespace,
         AnalysisCacheOwnerOptions options)
        : databasePath_(std::move(databasePath)), cacheNamespace_(std::move(cacheNamespace)),
          options_(options) {}

    ~Impl() { shutdown(); }

    [[nodiscard]] bool launch(QString* errorMessage) {
        try {
            worker_ = std::thread([this] { run(); });
            return true;
        } catch (const std::system_error& error) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Unable to start analysis cache worker: %1")
                                    .arg(QString::fromUtf8(error.what()));
            }
            return false;
        }
    }

    void waitUntilReady() {
        std::unique_lock lock(mutex_);
        readyCondition_.wait(lock, [this] { return ready_; });
    }

    void joinFailedStart() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    template <typename Result, typename Work, typename Failure>
    [[nodiscard]] AnalysisCacheOwnerSubmission<Result>
    submit(std::size_t retainedBytes, Work work, Failure failure) {
        auto promise = std::make_shared<std::promise<Result>>();
        std::future<Result> future = promise->get_future();

        std::lock_guard lock(mutex_);
        if (!accepting_) {
            return {AnalysisCacheOwnerSubmitStatus::ShuttingDown, {},
                    QStringLiteral("Analysis cache owner is shutting down")};
        }
        if (outstandingRequests_ >= options_.maximumOutstandingRequests ||
            retainedBytes > options_.maximumRetainedWriteBytes - outstandingRetainedBytes_) {
            return {AnalysisCacheOwnerSubmitStatus::QueueFull, {},
                    QStringLiteral("Analysis cache owner queue is full")};
        }

        auto workHolder = std::make_shared<std::decay_t<Work>>(std::move(work));
        auto failureHolder = std::make_shared<std::decay_t<Failure>>(std::move(failure));
        Task task;
        task.sequence = ++acceptedSequence_;
        task.retainedBytes = retainedBytes;
        task.execute = [promise, workHolder](
                           core::PagedCache& cache,
                           const AnalysisCacheNamespace& cacheNamespace) {
            auto result = std::make_shared<Result>((*workHolder)(cache, cacheNamespace));
            return std::function<void()>([promise, result] {
                promise->set_value(std::move(*result));
            });
        };
        task.fail = [promise, failureHolder](const QString& message) {
            auto result = std::make_shared<Result>((*failureHolder)(message));
            return std::function<void()>([promise, result] {
                promise->set_value(std::move(*result));
            });
        };
        tasks_.push_back(std::move(task));
        ++outstandingRequests_;
        outstandingRetainedBytes_ += retainedBytes;
        workCondition_.notify_one();
        return {AnalysisCacheOwnerSubmitStatus::Accepted, std::move(future), {}};
    }

    [[nodiscard]] AnalysisCacheOwnerFlushStatus flush() {
        std::unique_lock lock(mutex_);
        if (stopped_) {
            return AnalysisCacheOwnerFlushStatus::ShutDown;
        }
        const quint64 target = acceptedSequence_;
        drainCondition_.wait(lock, [this, target] {
            return completedSequence_ >= target || stopped_;
        });
        return completedSequence_ >= target ? AnalysisCacheOwnerFlushStatus::Drained
                                            : AnalysisCacheOwnerFlushStatus::ShutDown;
    }

    void shutdown() {
        std::call_once(shutdownOnce_, [this] {
            {
                std::lock_guard lock(mutex_);
                accepting_ = false;
                shutdownRequested_ = true;
            }
            workCondition_.notify_one();
            if (worker_.joinable()) {
                worker_.join();
            }
        });
    }

    [[nodiscard]] core::PagedCacheOpenStatus openStatus() const noexcept {
        return openStatus_;
    }
    [[nodiscard]] quint64 recoveredIncompleteBatchCount() const noexcept {
        return recoveredIncompleteBatchCount_;
    }
    [[nodiscard]] const QString& openErrorMessage() const noexcept { return openErrorMessage_; }
    [[nodiscard]] bool opened() const noexcept {
        return openStatus_ == core::PagedCacheOpenStatus::Opened;
    }
    [[nodiscard]] const AnalysisCacheNamespace& cacheNamespace() const noexcept {
        return cacheNamespace_;
    }

private:
    struct Task final {
        quint64 sequence = 0;
        std::size_t retainedBytes = 0;
        std::function<std::function<void()>(core::PagedCache&,
                                            const AnalysisCacheNamespace&)>
            execute;
        std::function<std::function<void()>(const QString&)> fail;
    };

    void run() {
        core::PagedCacheOpenResult opened =
            core::PagedCache::open(databasePath_, cacheNamespace_.name());
        {
            std::lock_guard lock(mutex_);
            openStatus_ = opened.status;
            recoveredIncompleteBatchCount_ = opened.recoveredIncompleteBatchCount;
            openErrorMessage_ = std::move(opened.errorMessage);
            accepting_ = opened.succeeded();
            ready_ = true;
            if (!opened.succeeded()) {
                stopped_ = true;
            }
        }
        readyCondition_.notify_one();
        if (!opened.succeeded()) {
            drainCondition_.notify_all();
            return;
        }

        std::unique_ptr<core::PagedCache> cache = std::move(opened.cache);
        for (;;) {
            Task task;
            {
                std::unique_lock lock(mutex_);
                workCondition_.wait(lock, [this] {
                    return shutdownRequested_ || !tasks_.empty();
                });
                if (tasks_.empty()) {
                    if (shutdownRequested_) {
                        break;
                    }
                    continue;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }

            std::function<void()> fulfill;
            try {
                fulfill = task.execute(*cache, cacheNamespace_);
            } catch (const std::exception& error) {
                fulfill = task.fail(QStringLiteral("Analysis cache worker request failed: %1")
                                        .arg(QString::fromUtf8(error.what())));
            } catch (...) {
                fulfill = task.fail(QStringLiteral("Analysis cache worker request failed"));
            }
            task.execute = {};
            task.fail = {};

            {
                std::lock_guard lock(mutex_);
                --outstandingRequests_;
                outstandingRetainedBytes_ -= task.retainedBytes;
            }
            fulfill();
            {
                std::lock_guard lock(mutex_);
                completedSequence_ = task.sequence;
            }
            drainCondition_.notify_all();
        }

        cache.reset();
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        drainCondition_.notify_all();
    }

    QString databasePath_;
    AnalysisCacheNamespace cacheNamespace_;
    AnalysisCacheOwnerOptions options_;
    std::mutex mutex_;
    std::condition_variable readyCondition_;
    std::condition_variable workCondition_;
    std::condition_variable drainCondition_;
    std::deque<Task> tasks_;
    std::thread worker_;
    std::once_flag shutdownOnce_;
    core::PagedCacheOpenStatus openStatus_ = core::PagedCacheOpenStatus::OpenFailed;
    quint64 recoveredIncompleteBatchCount_ = 0;
    QString openErrorMessage_;
    quint64 acceptedSequence_ = 0;
    quint64 completedSequence_ = 0;
    std::size_t outstandingRequests_ = 0;
    std::size_t outstandingRetainedBytes_ = 0;
    bool ready_ = false;
    bool accepting_ = false;
    bool shutdownRequested_ = false;
    bool stopped_ = false;
};

AnalysisCacheOwner::AnalysisCacheOwner(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

AnalysisCacheOwner::~AnalysisCacheOwner() { shutdown(); }

AnalysisCacheOwnerStartResult AnalysisCacheOwner::start(
    const QString& databasePath,
    AnalysisCacheNamespace cacheNamespace,
    AnalysisCacheOwnerOptions options) {
    if (databasePath.isEmpty() || options.maximumOutstandingRequests == 0 ||
        options.maximumOutstandingRequests > maximumOutstandingRequests() ||
        options.maximumRetainedWriteBytes == 0 ||
        options.maximumRetainedWriteBytes > maximumRetainedWriteBytes()) {
        return {AnalysisCacheOwnerStartStatus::InvalidArgument, {}, std::nullopt, 0,
                QStringLiteral("Analysis cache owner options or database path are invalid")};
    }

    auto impl =
        std::make_unique<Impl>(databasePath, std::move(cacheNamespace), options);
    QString workerError;
    if (!impl->launch(&workerError)) {
        return {AnalysisCacheOwnerStartStatus::WorkerStartFailed, {}, std::nullopt, 0,
                std::move(workerError)};
    }
    impl->waitUntilReady();
    if (!impl->opened()) {
        const auto status = impl->openStatus();
        const quint64 recovered = impl->recoveredIncompleteBatchCount();
        QString error = impl->openErrorMessage();
        impl->joinFailedStart();
        return {AnalysisCacheOwnerStartStatus::CacheOpenFailed, {}, status, recovered,
                std::move(error)};
    }
    const quint64 recovered = impl->recoveredIncompleteBatchCount();
    return {AnalysisCacheOwnerStartStatus::Started,
            std::unique_ptr<AnalysisCacheOwner>(new AnalysisCacheOwner(std::move(impl))),
            core::PagedCacheOpenStatus::Opened, recovered, {}};
}

AnalysisCacheOwnerWriteSubmission AnalysisCacheOwner::writeProgressiveIndex(
    std::vector<H264ProgressiveIndexCachePage> pages) {
    PreparedWriteBatch prepared = prepareWriteBatch(
        impl_->cacheNamespace(), std::move(pages), core::PagedCachePageKind::ProgressiveIndex,
        [](const H264ProgressiveIndexCachePage& page) {
            return AnalysisCachePageBodyCodec::encodeProgressiveIndex(page);
        });
    if (!prepared.succeeded()) {
        return {prepared.status, {}, std::move(prepared.errorMessage)};
    }
    const std::size_t retainedBytes = prepared.retainedBytes;
    auto writes = std::make_shared<std::vector<core::PagedCachePageWrite>>(
        std::move(prepared.writes));
    return impl_->submit<AnalysisCacheOwnerWriteResult>(
        retainedBytes,
        [writes](core::PagedCache& cache, const AnalysisCacheNamespace&) {
            core::PagedCacheCommitResult committed = cache.commitBatch(*writes);
            AnalysisCacheOwnerWriteResult result;
            result.cacheStatus = committed.status;
            result.errorMessage = std::move(committed.errorMessage);
            result.status = committed.succeeded() ? AnalysisCacheOwnerWriteStatus::Committed
                                                  : AnalysisCacheOwnerWriteStatus::StorageError;
            return result;
        },
        workerFailureWriteResult);
}

AnalysisCacheOwnerWriteSubmission AnalysisCacheOwner::writeMaterializedResult(
    std::vector<MaterializedResultCachePage> pages) {
    PreparedWriteBatch prepared = prepareWriteBatch(
        impl_->cacheNamespace(), std::move(pages), core::PagedCachePageKind::MaterializedResult,
        [](const MaterializedResultCachePage& page) {
            return AnalysisCachePageBodyCodec::encodeMaterializedResult(page);
        });
    if (!prepared.succeeded()) {
        return {prepared.status, {}, std::move(prepared.errorMessage)};
    }
    const std::size_t retainedBytes = prepared.retainedBytes;
    auto writes = std::make_shared<std::vector<core::PagedCachePageWrite>>(
        std::move(prepared.writes));
    return impl_->submit<AnalysisCacheOwnerWriteResult>(
        retainedBytes,
        [writes](core::PagedCache& cache, const AnalysisCacheNamespace&) {
            core::PagedCacheCommitResult committed = cache.commitBatch(*writes);
            AnalysisCacheOwnerWriteResult result;
            result.cacheStatus = committed.status;
            result.errorMessage = std::move(committed.errorMessage);
            result.status = committed.succeeded() ? AnalysisCacheOwnerWriteStatus::Committed
                                                  : AnalysisCacheOwnerWriteStatus::StorageError;
            return result;
        },
        workerFailureWriteResult);
}

H264ProgressiveIndexCacheReadSubmission AnalysisCacheOwner::readProgressiveIndex(
    core::PagedCachePageKey key) {
    if (!validReadKey(key, core::PagedCachePageKind::ProgressiveIndex)) {
        return {AnalysisCacheOwnerSubmitStatus::InvalidArgument, {},
                QStringLiteral("Progressive-index cache read key is invalid")};
    }
    return impl_->submit<H264ProgressiveIndexCacheReadResult>(
        0,
        [key](core::PagedCache& cache, const AnalysisCacheNamespace& cacheNamespace) {
            H264ProgressiveIndexCacheReadResult result;
            core::PagedCacheReadResult stored = cache.readPage(key);
            result.cacheStatus = stored.status;
            if (stored.status == core::PagedCacheReadStatus::Missing) {
                result.status = AnalysisCacheOwnerReadStatus::Missing;
                return result;
            }
            if (!stored.found()) {
                result.status = stored.status == core::PagedCacheReadStatus::InvalidRequest
                                    ? AnalysisCacheOwnerReadStatus::InvalidRequest
                                    : AnalysisCacheOwnerReadStatus::StorageError;
                result.errorMessage = std::move(stored.errorMessage);
                return result;
            }
            AnalysisCacheEnvelopeDecodeResult envelope = AnalysisCachePayloadEnvelope::decode(
                cacheNamespace, core::PagedCachePageKind::ProgressiveIndex, stored.bytes);
            result.envelopeStatus = envelope.status;
            if (!envelope.succeeded()) {
                result.status = AnalysisCacheOwnerReadStatus::Corrupt;
                result.errorMessage = std::move(envelope.errorMessage);
                return result;
            }
            H264ProgressiveIndexCacheDecodeResult decoded =
                AnalysisCachePageBodyCodec::decodeProgressiveIndex(key, envelope.payload);
            result.bodyStatus = decoded.status;
            if (!decoded.succeeded()) {
                result.status = AnalysisCacheOwnerReadStatus::Corrupt;
                result.errorMessage = std::move(decoded.errorMessage);
                return result;
            }
            result.status = AnalysisCacheOwnerReadStatus::Found;
            result.page = std::move(decoded.page);
            return result;
        },
        workerFailureProgressiveReadResult);
}

MaterializedResultCacheReadSubmission AnalysisCacheOwner::readMaterializedResult(
    core::PagedCachePageKey key) {
    if (!validReadKey(key, core::PagedCachePageKind::MaterializedResult)) {
        return {AnalysisCacheOwnerSubmitStatus::InvalidArgument, {},
                QStringLiteral("Materialized-result cache read key is invalid")};
    }
    return impl_->submit<MaterializedResultCacheReadResult>(
        0,
        [key](core::PagedCache& cache, const AnalysisCacheNamespace& cacheNamespace) {
            MaterializedResultCacheReadResult result;
            core::PagedCacheReadResult stored = cache.readPage(key);
            result.cacheStatus = stored.status;
            if (stored.status == core::PagedCacheReadStatus::Missing) {
                result.status = AnalysisCacheOwnerReadStatus::Missing;
                return result;
            }
            if (!stored.found()) {
                result.status = stored.status == core::PagedCacheReadStatus::InvalidRequest
                                    ? AnalysisCacheOwnerReadStatus::InvalidRequest
                                    : AnalysisCacheOwnerReadStatus::StorageError;
                result.errorMessage = std::move(stored.errorMessage);
                return result;
            }
            AnalysisCacheEnvelopeDecodeResult envelope = AnalysisCachePayloadEnvelope::decode(
                cacheNamespace, core::PagedCachePageKind::MaterializedResult, stored.bytes);
            result.envelopeStatus = envelope.status;
            if (!envelope.succeeded()) {
                result.status = AnalysisCacheOwnerReadStatus::Corrupt;
                result.errorMessage = std::move(envelope.errorMessage);
                return result;
            }
            MaterializedResultCacheDecodeResult decoded =
                AnalysisCachePageBodyCodec::decodeMaterializedResult(key, envelope.payload);
            result.bodyStatus = decoded.status;
            if (!decoded.succeeded()) {
                result.status = AnalysisCacheOwnerReadStatus::Corrupt;
                result.errorMessage = std::move(decoded.errorMessage);
                return result;
            }
            result.status = AnalysisCacheOwnerReadStatus::Found;
            result.page = std::move(decoded.page);
            return result;
        },
        workerFailureMaterializedReadResult);
}

AnalysisCacheOwnerFlushStatus AnalysisCacheOwner::flush() { return impl_->flush(); }

void AnalysisCacheOwner::shutdown() {
    if (impl_) {
        impl_->shutdown();
    }
}

} // namespace streamview::rules
