#pragma once

#include <QString>
#include <QtGlobal>

#include <compare>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace streamview::core {

enum class PagedCachePageKind : quint8 {
    ProgressiveIndex,
    MaterializedResult,
};

struct PagedCachePageKey final {
    PagedCachePageKind kind = PagedCachePageKind::ProgressiveIndex;
    quint64 streamId = 0;
    quint64 pageIndex = 0;

    friend constexpr auto operator<=>(const PagedCachePageKey&, const PagedCachePageKey&) = default;
};

struct PagedCachePageWrite final {
    PagedCachePageKey key;
    std::vector<std::byte> bytes;
};

enum class PagedCacheOpenStatus : quint8 {
    Opened,
    MissingSqliteDriver,
    InvalidArgument,
    OpenFailed,
    IncompatibleSchema,
    CorruptStore,
    StorageError,
};

enum class PagedCacheReadStatus : quint8 {
    Found,
    Missing,
    InvalidRequest,
    ThreadViolation,
    StorageError,
};

enum class PagedCacheCommitStatus : quint8 {
    Committed,
    InvalidBatch,
    ThreadViolation,
    StorageError,
};

struct PagedCacheReadResult final {
    PagedCacheReadStatus status = PagedCacheReadStatus::InvalidRequest;
    std::vector<std::byte> bytes;
    QString errorMessage;

    [[nodiscard]] bool found() const noexcept { return status == PagedCacheReadStatus::Found; }
};

struct PagedCacheCommitResult final {
    PagedCacheCommitStatus status = PagedCacheCommitStatus::InvalidBatch;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == PagedCacheCommitStatus::Committed;
    }
};

struct PagedCacheOpenResult;

class PagedCache final {
  public:
    [[nodiscard]] static constexpr quint32 schemaVersion() noexcept { return 1; }
    [[nodiscard]] static constexpr std::size_t pageSizeBytes() noexcept { return 64U * 1024U; }
    [[nodiscard]] static constexpr std::size_t maximumBatchPages() noexcept { return 256; }
    [[nodiscard]] static constexpr qsizetype maximumNamespaceLength() noexcept { return 512; }

    [[nodiscard]] static PagedCacheOpenResult open(const QString& databasePath,
                                                   const QString& namespaceName);

    ~PagedCache();
    PagedCache(const PagedCache&) = delete;
    PagedCache& operator=(const PagedCache&) = delete;
    PagedCache(PagedCache&&) = delete;
    PagedCache& operator=(PagedCache&&) = delete;

    [[nodiscard]] PagedCacheReadResult readPage(const PagedCachePageKey& key) const;
    [[nodiscard]] PagedCacheCommitResult commitBatch(std::span<const PagedCachePageWrite> pages);

  private:
    class Impl;
    explicit PagedCache(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

struct PagedCacheOpenResult final {
    PagedCacheOpenStatus status = PagedCacheOpenStatus::OpenFailed;
    std::unique_ptr<PagedCache> cache;
    quint64 recoveredIncompleteBatchCount = 0;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == PagedCacheOpenStatus::Opened && cache != nullptr;
    }
};

} // namespace streamview::core
