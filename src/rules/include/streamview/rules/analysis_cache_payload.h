#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/paged_cache.h>
#include <streamview/rules/analysis_cache.h>
#include <streamview/rules/h264_start_code_scanner.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace streamview::rules {

struct H264ProgressiveIndexCachePage final {
    core::PagedCachePageKey key{core::PagedCachePageKind::ProgressiveIndex, 0, 0};
    quint64 firstRecordIndex = 0;
    quint64 indexedThroughByteOffset = 0;
    bool endOfSource = false;
    std::vector<H264StartCodeRecord> records;
};

struct MaterializedResultCacheNode final {
    core::AnalysisNodeId id;
    std::optional<core::AnalysisNodeId> parentId;
    core::AnalysisNodeSpec spec;
    std::vector<core::ParseDiagnostic> diagnostics;
};

struct MaterializedResultCachePage final {
    core::PagedCachePageKey key{core::PagedCachePageKind::MaterializedResult, 0, 0};
    std::vector<MaterializedResultCacheNode> nodes;
};

enum class AnalysisCacheBodyEncodeStatus : quint8 {
    Encoded,
    InvalidArgument,
    UnsupportedValue,
    PayloadTooLarge,
};

struct AnalysisCacheBodyEncodeResult final {
    AnalysisCacheBodyEncodeStatus status = AnalysisCacheBodyEncodeStatus::InvalidArgument;
    std::vector<std::byte> bytes;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == AnalysisCacheBodyEncodeStatus::Encoded;
    }
};

enum class AnalysisCacheBodyDecodeStatus : quint8 {
    Decoded,
    InvalidBody,
    PageKeyMismatch,
    UnsupportedVersion,
    UnsupportedValue,
};

struct H264ProgressiveIndexCacheDecodeResult final {
    AnalysisCacheBodyDecodeStatus status = AnalysisCacheBodyDecodeStatus::InvalidBody;
    std::optional<H264ProgressiveIndexCachePage> page;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == AnalysisCacheBodyDecodeStatus::Decoded && page.has_value();
    }
};

struct MaterializedResultCacheDecodeResult final {
    AnalysisCacheBodyDecodeStatus status = AnalysisCacheBodyDecodeStatus::InvalidBody;
    std::optional<MaterializedResultCachePage> page;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == AnalysisCacheBodyDecodeStatus::Decoded && page.has_value();
    }
};

enum class MaterializedResultCacheExportStatus : quint8 {
    Exported,
    InvalidTree,
    UnsupportedValue,
    PayloadTooLarge,
    TooManyPages,
};

struct MaterializedResultCacheExportResult final {
    MaterializedResultCacheExportStatus status =
        MaterializedResultCacheExportStatus::InvalidTree;
    std::vector<MaterializedResultCachePage> pages;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == MaterializedResultCacheExportStatus::Exported && !pages.empty();
    }
};

class AnalysisCachePageBodyCodec final {
public:
    [[nodiscard]] static constexpr quint32 progressiveIndexFormatVersion() noexcept {
        return 1;
    }
    [[nodiscard]] static constexpr quint32 materializedResultFormatVersion() noexcept {
        return 1;
    }

    [[nodiscard]] static AnalysisCacheBodyEncodeResult
    encodeProgressiveIndex(const H264ProgressiveIndexCachePage& page);
    [[nodiscard]] static H264ProgressiveIndexCacheDecodeResult
    decodeProgressiveIndex(const core::PagedCachePageKey& expectedKey,
                           std::span<const std::byte> bytes);

    [[nodiscard]] static AnalysisCacheBodyEncodeResult
    encodeMaterializedResult(const MaterializedResultCachePage& page);
    [[nodiscard]] static MaterializedResultCacheDecodeResult
    decodeMaterializedResult(const core::PagedCachePageKey& expectedKey,
                             std::span<const std::byte> bytes);
};

[[nodiscard]] MaterializedResultCacheExportResult
exportMaterializedResultCachePages(const core::AnalysisTree& tree,
                                   quint64 streamId,
                                   quint64 firstPageIndex = 0);

} // namespace streamview::rules
